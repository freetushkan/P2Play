// Covers: a torrent added without a poster (a magnet, a .torrent file, a torrs:// card without
// one) gets the picture found inside it, as P2Play does. First the image files of the
// torrent itself (poster.*, cover.*, folder.* first, then other images), then embedded
// attached_pic artwork, then — when CoverProbe is on — a representative video frame, read
// through the core's own stream route over loopback.
//
// The result is a disk cache by infohash, <data>/covers/<infohash>.img with <infohash>.json
// ({"source","type","bytes","at"}); a miss is remembered in <data>/covers/<infohash>.none so a
// second start does not fetch the same torrent again. A poster the client sent is never
// replaced: the cache only fills empty ones, so pPlay/Silo keep owning the posters they have.
//
// The jobs run on their own thread, never on an HTTP or libtorrent thread: a cover of a
// torrent without peers takes as long as its metadata, and the API keeps answering meanwhile.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "core_torrent.hpp"

namespace p2p {

    using nlohmann::json;
    using Clock = std::chrono::steady_clock;

    namespace {
        constexpr size_t kMaxCoverBytes = 2u << 20;          // as a MediaMesh poster
        constexpr int64_t kMaxCoverSourceBytes = 32u << 20;  // a picture bigger than this is not one
        constexpr auto kCoverInfoTimeout = std::chrono::seconds(120);
        constexpr int kFfmpegTimeout = 45;                   // s, killed after
        constexpr size_t kCoverQueueLimit = 64;

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        std::string baseName(const std::string &path) {
            const size_t slash = path.rfind('/');
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }

        // the image type of these bytes: "" if they are not a picture the browser shows
        std::string imageType(const std::string &b) {
            if (b.size() > 3 && static_cast<unsigned char>(b[0]) == 0xff &&
                static_cast<unsigned char>(b[1]) == 0xd8 && static_cast<unsigned char>(b[2]) == 0xff) {
                return "image/jpeg";
            }
            if (b.size() >= 8 && b.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0) return "image/png";
            if (b.size() > 12 && b.compare(0, 4, "RIFF", 4) == 0 && b.compare(8, 4, "WEBP", 4) == 0) {
                return "image/webp";
            }
            if (b.size() > 6 && (b.compare(0, 6, "GIF87a", 6) == 0 || b.compare(0, 6, "GIF89a", 6) == 0)) {
                return "image/gif";
            }
            return "";
        }

        // how much a file name looks like the poster of the release; 0: not a candidate
        int coverNameScore(const std::string &path) {
            const std::string name = lower(baseName(path));
            const size_t dot = name.rfind('.');
            const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
            const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
            if (ext != "jpg" && ext != "jpeg" && ext != "png" && ext != "webp" && ext != "gif") return 0;
            if (stem == "poster") return 100;
            if (stem == "cover") return 95;
            if (stem == "folder") return 90;
            if (stem == "front") return 85;
            if (stem == "fanart" || stem == "backdrop") return 80;
            if (stem == "thumb" || stem == "thumbnail") return 70;
            if (stem.rfind("poster", 0) == 0 || stem.rfind("cover", 0) == 0) return 60;
            // any other picture of the release: after the named ones, the smallest first
            return 10;
        }

        bool isVideoName(const std::string &path) {
            const std::string name = lower(baseName(path));
            const size_t dot = name.rfind('.');
            const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
            return ext == "mkv" || ext == "mp4" || ext == "m4v" || ext == "avi" || ext == "ts" ||
                   ext == "m2ts" || ext == "mov" || ext == "wmv" || ext == "mpg" || ext == "mpeg" ||
                   ext == "webm" || ext == "flv" || ext == "ogv";
        }

        std::string coverPath(const std::string &dataDir, const std::string &hash) {
            return dataDir + "/covers/" + lower(hash);
        }

        std::string randomToken() {
            std::random_device device;
            std::mt19937_64 engine((static_cast<uint64_t>(device()) << 32) ^ device() ^
                                   static_cast<uint64_t>(std::chrono::high_resolution_clock::now()
                                                                 .time_since_epoch().count()));
            static const char *digits = "0123456789abcdef";
            std::string out;
            for (int i = 0; i < 32; i++) {
                out += digits[engine() & 15];
                out += digits[(engine() >> 8) & 15];
            }
            return out;
        }

        bool fileExists(const std::string &path) {
            struct stat st{};
            return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
        }

        bool readWhole(const std::string &path, std::string &out) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            std::stringstream ss;
            ss << in.rdbuf();
            out = ss.str();
            return true;
        }

        bool writeWhole(const std::string &path, const std::string &data) {
            const std::string tmp = path + ".tmp";
            FILE *f = std::fopen(tmp.c_str(), "wb");
            if (f == nullptr) return false;
            const bool written = std::fwrite(data.data(), 1, data.size(), f) == data.size();
            const bool closed = std::fclose(f) == 0;
            if (!written || !closed || std::rename(tmp.c_str(), path.c_str()) != 0) {
                std::remove(tmp.c_str());
                return false;
            }
            return true;
        }

        void makeDirs(const std::string &path) {
            for (size_t i = 1; i <= path.size(); i++) {
                if (i == path.size() || path[i] == '/') {
                    const std::string part = path.substr(0, i);
                    if (!part.empty()) ::mkdir(part.c_str(), 0700);
                }
            }
        }

        bool onPath(const std::string &name) {
            const char *env = getenv("PATH");
            if (env == nullptr) return false;
            const std::string paths(env);
            size_t pos = 0;
            while (pos <= paths.size()) {
                size_t end = paths.find(':', pos);
                if (end == std::string::npos) end = paths.size();
                const std::string dir = paths.substr(pos, end - pos);
                if (!dir.empty() && fileExists(dir + "/" + name)) return true;
                pos = end + 1;
            }
            return false;
        }

#if defined(__linux__) || defined(__ANDROID__)
        // ffmpeg with a deadline; true when it ran and exited well (the caller checks the file)
        bool runFfmpeg(const std::vector<std::string> &args, int timeoutSeconds) {
            std::vector<char *> argv;
            argv.reserve(args.size() + 1);
            for (const std::string &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
            argv.push_back(nullptr);
            const pid_t pid = fork();
            if (pid < 0) return false;
            if (pid == 0) {
                const int devnull = open("/dev/null", O_RDWR);
                if (devnull >= 0) {
                    dup2(devnull, STDIN_FILENO);
                    dup2(devnull, STDOUT_FILENO);
                    dup2(devnull, STDERR_FILENO);
                    if (devnull > STDERR_FILENO) close(devnull);
                }
                execvp(argv[0], argv.data());
                _exit(127);
            }
            const auto until = Clock::now() + std::chrono::seconds(timeoutSeconds);
            int status = 0;
            while (Clock::now() < until) {
                const pid_t done = waitpid(pid, &status, WNOHANG);
                if (done == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
                if (done < 0) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return false;
        }
        // ffprobe writes a small JSON stream inventory to a private temporary file.
        bool runFfprobe(const std::vector<std::string> &args, const std::string &outputPath, int timeoutSeconds) {
            std::vector<char *> argv;
            argv.reserve(args.size() + 1);
            for (const std::string &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
            argv.push_back(nullptr);
            const pid_t pid = fork();
            if (pid < 0) return false;
            if (pid == 0) {
                const int output = open(outputPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (output < 0) _exit(127);
                dup2(output, STDOUT_FILENO);
                close(output);
                const int devnull = open("/dev/null", O_RDWR);
                if (devnull >= 0) {
                    dup2(devnull, STDIN_FILENO);
                    dup2(devnull, STDERR_FILENO);
                    if (devnull > STDERR_FILENO) close(devnull);
                }
                execvp(argv[0], argv.data());
                _exit(127);
            }
            const auto until = Clock::now() + std::chrono::seconds(timeoutSeconds);
            int status = 0;
            while (Clock::now() < until) {
                const pid_t done = waitpid(pid, &status, WNOHANG);
                if (done == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
                if (done < 0) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return false;
        }
#else
        // no external binaries in the console packages this core is built into
        bool runFfmpeg(const std::vector<std::string> &, int) { return false; }
        bool runFfprobe(const std::vector<std::string> &, const std::string &, int) { return false; }
#endif
    }

    // the per-run token the core's own ffmpeg presents on the stream route (127.0.0.1 only)
    void Core::coverInit() {
        m_internalToken = randomToken();
    }

    void Core::coverRequest(const std::string &hashIn) {
        const std::string hash = lower(hashIn);
        if (hash.empty()) return;
        {
            std::lock_guard<std::mutex> lock(m_coverMutex);
            if (m_coverQueued.count(hash) != 0 || m_coverQueue.size() >= kCoverQueueLimit) return;
            m_coverQueued.insert(hash);
            m_coverQueue.push_back(hash);
        }
        m_coverChanged.notify_all();
    }

    bool Core::coverInfo(const std::string &hash, std::string &source) {
        const std::string base = coverPath(m_store.dir(), hash);
        std::string meta;
        if (!fileExists(base + ".img") || !readWhole(base + ".json", meta)) return false;
        const json j = json::parse(meta, nullptr, false);
        if (!j.is_object()) return false;
        source = j.value("source", std::string());
        return true;
    }

    bool Core::coverRead(const std::string &hash, std::string &bytes, std::string &type, std::string &source) {
        const std::string base = coverPath(m_store.dir(), hash);
        std::string meta;
        if (!readWhole(base + ".json", meta)) return false;
        const json j = json::parse(meta, nullptr, false);
        if (!j.is_object()) return false;
        if (!readWhole(base + ".img", bytes) || bytes.empty()) return false;
        type = j.value("type", std::string("application/octet-stream"));
        source = j.value("source", std::string());
        return true;
    }


    bool Core::coverValidate(const std::string &bytes, std::string &type) const {
        type = imageType(bytes);
        return !type.empty() && !bytes.empty() && bytes.size() <= kMaxCoverBytes;
    }

    // Only public IPv4 HTTP endpoints are fetched: poster URLs must not turn this route into
    // an SSRF primitive against loopback, LAN services, link-local or reserved networks.
    bool Core::fetchUrl(const std::string &input, size_t maxBytes, std::string &bytes,
                        std::string &type, std::string &error) {
        bytes.clear();
        type.clear();
        error.clear();
        if (maxBytes == 0 || maxBytes > 16u * 1024u * 1024u) {
            error = "HTTP(S) response size limit is invalid"; return false;
        }
        if (input.size() > 2048 || input.find_first_of("\r\n\t ") != std::string::npos) {
            error = "HTTP(S) URL is invalid or too long"; return false;
        }

        bool requireHttps = input.rfind("https://", 0) == 0;
        std::string url = input;
        auto setRedirect = [&](std::string target, const std::string &hostPort, bool sourceHttps) {
            const size_t first = target.find_first_not_of(" \t");
            if (first == std::string::npos) { error = "HTTP(S) redirect has an empty Location"; return false; }
            const size_t last = target.find_last_not_of(" \t\r\n");
            target = target.substr(first, last - first + 1);
            if (target.rfind("http://", 0) == 0 || target.rfind("https://", 0) == 0) url = target;
            else if (target.rfind("//", 0) == 0) url = std::string(sourceHttps ? "https:" : "http:") + target;
            else if (!target.empty() && target[0] == '/') url = std::string(sourceHttps ? "https://" : "http://") + hostPort + target;
            else { error = "HTTP(S) redirect target is invalid"; return false; }
            if (url.rfind("https://", 0) == 0) requireHttps = true;
            return true;
        };

        for (int hop = 0; hop < 4; ++hop) {
            const bool https = url.rfind("https://", 0) == 0;
            if (requireHttps && !https) { error = "HTTP(S) redirect cannot downgrade HTTPS to HTTP"; return false; }
            const bool http = url.rfind("http://", 0) == 0;
            if (!https && !http) { error = "URL must use HTTP or HTTPS"; return false; }
            if (https && !m_config.httpGet) { error = "HTTPS fetching requires a host TLS transport"; return false; }

            const size_t schemeSize = https ? 8 : 7;
            const std::string rest = url.substr(schemeSize);
            const size_t slash = rest.find_first_of("/?#");
            const std::string hostPort = rest.substr(0, slash);
            std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
            const size_t fragment = path.find('#');
            if (fragment != std::string::npos) path.resize(fragment);
            if (path.empty()) path = "/";
            if (path[0] == '?') path = "/" + path;
            if (hostPort.empty() || hostPort.find('@') != std::string::npos ||
                hostPort.find_first_of("\\\r\n\t ") != std::string::npos ||
                path.find_first_of("\r\n") != std::string::npos) {
                error = "HTTP(S) URL host or path is invalid"; return false;
            }

            std::string host = hostPort, port = https ? "443" : "80";
            const size_t colon = hostPort.rfind(':');
            if (colon != std::string::npos) { host = hostPort.substr(0, colon); port = hostPort.substr(colon + 1); }
            if (host.empty() || port.empty() ||
                !std::all_of(port.begin(), port.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
                error = "HTTP(S) URL host or port is invalid"; return false;
            }
            const int portNumber = std::atoi(port.c_str());
            if (portNumber < 1 || portNumber > 65535) { error = "HTTP(S) URL port is invalid"; return false; }

            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo *addresses = nullptr;
            if (getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0 || !addresses) {
                error = "cannot resolve host " + host; return false;
            }

            int fd = -1;
            bool publicAddressFound = false;
            std::string pinnedResolve;
            for (addrinfo *address = addresses; address; address = address->ai_next) {
                const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(address->ai_addr);
                const uint32_t value = ntohl(ipv4->sin_addr.s_addr);
                const unsigned a = (value >> 24) & 255, b = (value >> 16) & 255, d = (value >> 8) & 255;
                const bool privateAddress = a == 0 || a == 10 || a == 127 || a >= 224 ||
                    (a == 169 && b == 254) || (a == 172 && b >= 16 && b <= 31) ||
                    (a == 192 && b == 168) || (a == 100 && b >= 64 && b <= 127) ||
                    (a == 192 && b == 0) || (a == 198 && (b == 18 || b == 19)) ||
                    (a == 198 && b == 51 && d == 100) || (a == 203 && b == 0 && d == 113);
                if (privateAddress) continue;
                publicAddressFound = true;
                if (m_config.httpGet) {
                    char ip[INET_ADDRSTRLEN] = {};
                    if (!inet_ntop(AF_INET, &ipv4->sin_addr, ip, sizeof(ip))) continue;
                    pinnedResolve = host + ":" + port + ":" + ip;
                    break;
                }
                fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
                if (fd < 0) continue;
                timeval timeout{10, 0};
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) break;
                ::close(fd); fd = -1;
            }
            freeaddrinfo(addresses);

            if (m_config.httpGet) {
                if (pinnedResolve.empty()) { error = "URL host is not publicly reachable"; return false; }
                std::string responseBytes, responseType, redirect, fetchError;
                const int status = m_config.httpGet(url, pinnedResolve, maxBytes, responseBytes,
                                                    responseType, redirect, fetchError);
                if (status >= 300 && status < 400) {
                    if (redirect.empty()) { error = "HTTP(S) redirect has no Location header"; return false; }
                    if (!setRedirect(redirect, hostPort, https)) return false;
                    continue;
                }
                if (status != 200) {
                    error = fetchError.empty() ? "HTTP(S) server returned HTTP " + std::to_string(status) : fetchError;
                    return false;
                }
                if (responseBytes.size() > maxBytes) { error = "HTTP(S) response exceeds the configured size limit"; return false; }
                bytes = std::move(responseBytes);
                type = responseType;
                return true;
            }

            if (fd < 0) {
                error = publicAddressFound ? "cannot connect to public host " + hostPort : "URL host is not publicly reachable";
                return false;
            }
            const std::string request = "GET " + path + " HTTP/1.0\r\nHost: " + hostPort +
                "\r\nUser-Agent: P2Play\r\nAccept: */*\r\nConnection: close\r\n\r\n";
            size_t sent = 0;
            while (sent < request.size()) {
                const ssize_t count = ::send(fd, request.data() + sent, request.size() - sent, 0);
                if (count <= 0) break;
                sent += static_cast<size_t>(count);
            }

            std::string response;
            char buffer[8192];
            ssize_t count = 0;
            const size_t responseLimit = maxBytes + 32768;
            bool responseOverflow = false;
            while (sent == request.size() && (count = recv(fd, buffer, sizeof(buffer), 0)) > 0) {
                if (response.size() + static_cast<size_t>(count) > responseLimit) { responseOverflow = true; break; }
                response.append(buffer, static_cast<size_t>(count));
            }
            ::close(fd);

            const size_t headEnd = response.find("\r\n\r\n");
            if (responseOverflow || response.empty() || headEnd == std::string::npos || headEnd > 32768) {
                error = "HTTP(S) server returned an invalid or oversized response"; return false;
            }
            const std::string head = response.substr(0, headEnd);
            const size_t statusSpace = head.find(' ');
            const int status = statusSpace == std::string::npos ? 0 : std::atoi(head.substr(statusSpace + 1, 3).c_str());
            if (status >= 300 && status < 400) {
                const std::string lowered = lower(head);
                const size_t location = lowered.find("\r\nlocation:");
                if (location == std::string::npos) { error = "HTTP(S) redirect has no Location header"; return false; }
                const size_t end = head.find("\r\n", location + 2);
                std::string target = head.substr(location + 11, end == std::string::npos ? std::string::npos : end - location - 11);
                if (!setRedirect(target, hostPort, https)) return false;
                continue;
            }
            if (status != 200) { error = "HTTP(S) server returned HTTP " + std::to_string(status); return false; }
            bytes = response.substr(headEnd + 4);
            if (bytes.size() > maxBytes) {
                bytes.clear(); error = "HTTP(S) response exceeds the configured size limit"; return false;
            }

            const std::string loweredHead = lower(head);
            const size_t contentTypeHeader = loweredHead.find("\r\ncontent-type:");
            if (contentTypeHeader != std::string::npos) {
                const size_t valueStart = head.find(':', contentTypeHeader) + 1;
                const size_t valueEnd = head.find("\r\n", valueStart);
                const std::string value = head.substr(valueStart, valueEnd == std::string::npos ? std::string::npos : valueEnd - valueStart);
                const size_t first = value.find_first_not_of(" \t");
                if (first != std::string::npos) type = value.substr(first);
            }
            return true;
        }
        error = "too many HTTP(S) redirects";
        return false;
    }

    bool Core::coverFetchUrl(const std::string &input, std::string &bytes,
                             std::string &type, std::string &error) {
        if (!fetchUrl(input, kMaxCoverBytes, bytes, type, error)) return false;
        if (!coverValidate(bytes, type)) {
            bytes.clear();
            error = "poster response is not a supported image or exceeds 2 MiB";
            return false;
        }
        return true;
    }

    bool Core::coverStore(const std::string &hash, const std::string &bytes,
                          const std::string &source, std::string &error) {
        std::string type;
        if (!coverValidate(bytes, type)) {
            error = "poster must be a JPEG, PNG, WebP or GIF image no larger than 2 MiB";
            return false;
        }
        const std::string base = coverPath(m_store.dir(), hash);
        const size_t slash = base.rfind('/');
        if (slash == std::string::npos) {
            error = "cannot determine poster storage directory";
            return false;
        }
        makeDirs(base.substr(0, slash));
        if (!writeWhole(base + ".img", bytes)) {
            error = "cannot save poster on the P2Play server";
            return false;
        }
        const json metadata = {{"source", source}, {"type", type}, {"bytes", bytes.size()},
                               {"at", static_cast<int64_t>(time(nullptr))}};
        if (!writeWhole(base + ".json", metadata.dump())) {
            std::remove((base + ".img").c_str());
            error = "cannot save poster metadata on the P2Play server";
            return false;
        }
        std::remove((base + ".none").c_str());
        return true;
    }

    void Core::coverForget(const std::string &hashIn) {
        const std::string base = coverPath(m_store.dir(), hashIn);
        std::remove((base + ".img").c_str());
        std::remove((base + ".json").c_str());
        std::remove((base + ".none").c_str());
        std::remove((base + ".probe.jpg").c_str());
        std::lock_guard<std::mutex> lock(m_coverMutex);
        m_coverQueued.erase(lower(hashIn));
    }

    // A byte range of a file, piece by piece through the cache (RAM or disk); fileId is the
    // core's own id (as stream() uses), not the API's zero-based index. The caller keeps the
    // torrent active, the reader window is released here.
    bool Core::fileBytes(const TorrentPtr &t, int fileId, int64_t offset, size_t length, std::string &out) {
        int pieceLength = 0;
        int64_t fileOffset = 0, fileSize = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!t->active || !t->gotInfo || t->fileIndex.count(fileId) == 0) return false;
            std::shared_ptr<const lt::torrent_info> ti = t->handle.torrent_file();
            if (!ti) return false;
            const lt::file_index_t fi = t->fileIndex.at(fileId);
            fileOffset = ti->files().file_offset(fi);
            fileSize = ti->files().file_size(fi);
            pieceLength = t->pieceLength;
        }
        if (pieceLength <= 0 || offset < 0 || offset >= fileSize || length == 0) return false;
        const int64_t end = std::min<int64_t>(fileSize, offset + static_cast<int64_t>(length));
        const int firstPiece = static_cast<int>((fileOffset + offset) / pieceLength);
        const int lastPiece = static_cast<int>((fileOffset + end - 1) / pieceLength);
        if (lastPiece - firstPiece > 64) return false;  // a picture, not a video

        const int reader = m_nextReader++;
        setWindow(t, reader, firstPiece, lastPiece);
        out.assign(static_cast<size_t>(end - offset), '\0');
        bool ok = true;
        size_t written = 0;
        for (int piece = firstPiece; piece <= lastPiece && ok; piece++) {
            std::shared_ptr<const std::vector<char>> data = readPiece(t, piece, std::chrono::seconds(90));
            if (!data) {
                ok = false;
                break;
            }
            const int64_t pieceStart = static_cast<int64_t>(piece) * pieceLength;
            const int64_t from = std::max<int64_t>(pieceStart, fileOffset + offset);
            const int64_t to = std::min<int64_t>(pieceStart + static_cast<int64_t>(data->size()), fileOffset + end);
            if (to <= from) continue;
            const size_t inPiece = static_cast<size_t>(from - pieceStart);
            const size_t count = static_cast<size_t>(to - from);
            if (inPiece + count > data->size() || written + count > out.size()) {
                ok = false;
                break;
            }
            std::memcpy(&out[written], data->data() + inPiece, count);
            written += count;
        }
        setWindow(t, reader, -1, -1);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            t->lastUse = Clock::now();
        }
        if (!ok || written != out.size()) {
            out.clear();
            return false;
        }
        return true;
    }

    // Source priority: a named image file, attached artwork inside a video, then an optional frame.
    bool Core::coverExtract(const TorrentPtr &t, std::string &bytes, std::string &type,
                            std::string &source, std::string &error) {
        std::vector<FileInfo> files;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            files = t->rec.files;
        }
        struct Candidate { int score; int64_t length; int id; std::string path; };
        std::vector<Candidate> pictures;
        if (m_settings.coverFiles) {
            for (const FileInfo &f : files) {
                const int score = coverNameScore(f.path);
                if (score == 0 || f.length <= 0 || f.length > kMaxCoverSourceBytes) continue;
                pictures.push_back({score, f.length, f.id, f.path});
            }
            std::sort(pictures.begin(), pictures.end(), [](const Candidate &a, const Candidate &b) {
                if (a.score != b.score) return a.score > b.score;
                if (a.length != b.length) return a.length < b.length;
                return a.path < b.path;
            });
            for (size_t i = 0; i < pictures.size() && i < 4; i++) {
                std::string data;
                const size_t want = static_cast<size_t>(
                        std::min<int64_t>(pictures[i].length, static_cast<int64_t>(kMaxCoverBytes)));
                if (!fileBytes(t, pictures[i].id, 0, want, data)) continue;
                const std::string kind = imageType(data);
                if (kind.empty()) continue;
                bytes = std::move(data);
                type = kind;
                source = "torrent:" + baseName(pictures[i].path);
                return true;
            }
        }

        const FileInfo *video = nullptr;
        for (const FileInfo &f : files) {
            if (!isVideoName(f.path) || f.length <= 0) continue;
            if (video == nullptr || f.length > video->length) video = &f;
        }
        if (video == nullptr) {
            error = "no picture file and no video inside the torrent";
            return false;
        }
        if (!m_settings.coverEmbedded && !m_settings.coverProbe) {
            error = "embedded artwork and frame extraction are disabled";
            return false;
        }
        if (!onPath("ffmpeg")) {
            error = "ffmpeg is not on PATH";
            return false;
        }
        if (m_http.port() == 0 || m_internalToken.empty()) {
            error = "cover extraction needs the HTTP server";
            return false;
        }

        const std::string hash = t->rec.hash;
        const std::string cacheDir = coverPath(m_store.dir(), hash);
        makeDirs(cacheDir.substr(0, cacheDir.rfind('/')));
        const std::string out = cacheDir + ".probe.jpg";
        const std::string probeOut = coverPath(m_store.dir(), hash) + ".probe.json";
        const int index = video->index >= 0 ? video->index : video->id - 1;
        const std::string url = "http://127.0.0.1:" + std::to_string(m_http.port()) + m_config.httpPrefix +
                                "/api/v1/torrents/" + hash + "/files/" + std::to_string(index) + "/stream";
        const std::string headers = "X-P2Play-Internal: " + m_internalToken + "\r\n";
        auto readImage = [&](const std::vector<std::string> &args, const std::string &sourceName) {
            std::remove(out.c_str());
            if (!runFfmpeg(args, kFfmpegTimeout)) return false;
            std::string image;
            if (!readWhole(out, image) || image.empty()) return false;
            const std::string kind = imageType(image);
            if (kind.empty()) return false;
            if (image.size() > kMaxCoverBytes) image.resize(kMaxCoverBytes);
            bytes = std::move(image);
            type = kind;
            source = sourceName;
            return true;
        };

        // Embedded cover art is a separate attached-picture stream, not a regular video frame.
        // ffprobe identifies that stream so the full video is never mistaken for its poster.
        if (m_settings.coverEmbedded && onPath("ffprobe")) {
            const std::vector<std::string> args = {"ffprobe", "-v", "error", "-select_streams", "v",
                                                    "-show_entries", "stream=index:stream_disposition=attached_pic",
                                                    "-of", "json", "-headers", headers, url};
            std::remove(probeOut.c_str());
            if (runFfprobe(args, probeOut, kFfmpegTimeout)) {
                std::string jsonText;
                if (readWhole(probeOut, jsonText)) {
                    const json inventory = json::parse(jsonText, nullptr, false);
                    const json streams = inventory.is_object() ? inventory.value("streams", json::array()) : json::array();
                    if (streams.is_array()) {
                        for (const json &stream : streams) {
                            if (!stream.is_object() || !stream.value("index", json()).is_number_integer()) continue;
                            const json disposition = stream.value("disposition", json::object());
                            if (!disposition.is_object()) continue;
                            const json attachedPic = disposition.value("attached_pic", json(false));
                            const bool attached = attachedPic.is_boolean()
                                                          ? attachedPic.get<bool>()
                                                          : attachedPic.is_number_integer() && attachedPic.get<int>() != 0;
                            if (!attached) continue;
                            const int streamIndex = stream["index"].get<int>();
                            const std::vector<std::string> extract = {"ffmpeg", "-nostdin", "-loglevel", "error", "-y",
                                "-headers", headers, "-i", url, "-map", "0:" + std::to_string(streamIndex),
                                "-frames:v", "1", "-vf", "scale=480:-2", "-f", "image2", out};
                            if (readImage(extract, "embedded:" + baseName(video->path))) {
                                std::remove(probeOut.c_str());
                                std::remove(out.c_str());
                                return true;
                            }
                        }
                    }
                }
            }
            std::remove(probeOut.c_str());
        }

        if (!m_settings.coverProbe) {
            error = m_settings.coverEmbedded
                    ? "no embedded artwork was found; video frame extraction is disabled"
                    : "video frame extraction is disabled";
            std::remove(out.c_str());
            return false;
        }

        for (const char *seek : {"30", "0"}) {
            const std::vector<std::string> args = {"ffmpeg", "-nostdin", "-loglevel", "error", "-y",
                "-headers", headers, "-ss", seek, "-i", url, "-frames:v", "1",
                "-vf", "scale=480:-2", "-f", "image2", out};
            if (readImage(args, "probe")) {
                std::remove(out.c_str());
                return true;
            }
        }
        std::remove(out.c_str());
        error = "ffmpeg found no picture";
        return false;
    }

    void Core::coverLoop() {
        while (!m_stopping) {
            std::string hash;
            {
                std::unique_lock<std::mutex> lock(m_coverMutex);
                if (!m_coverChanged.wait_for(lock, std::chrono::seconds(1), [this] {
                        return m_stopping.load() || !m_coverQueue.empty();
                    })) {
                    continue;
                }
                if (m_stopping) return;
                hash = m_coverQueue.front();
                m_coverQueue.pop_front();
                m_coverQueued.erase(hash);
            }
            try {
                TorrentPtr t = find(hash);
                std::string poster;
                bool inLibrary = false;
                if (t) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    poster = t->rec.poster;
                    inLibrary = !t->rec.users.empty();
                }
                const std::string base = coverPath(m_store.dir(), hash);
                if (!t || !inLibrary || !poster.empty() || fileExists(base + ".img") ||
                    fileExists(base + ".none")) {
                    continue;
                }
                std::string error, addError;
                if (!add(hash, "", "", "", "", "", addError)) {
                    error = addError.empty() ? "torrent could not be activated" : addError;
                } else if (!waitInfo(t, kCoverInfoTimeout)) {
                    error = "torrent metadata is not ready";
                }
                std::string bytes, type, source;
                if (error.empty()) coverExtract(t, bytes, type, source, error);
                if (!bytes.empty()) {
                    // Serialize the final write against API poster changes; user posters win.
                    std::lock_guard<std::mutex> posterLock(m_mutex);
                    if (!t->rec.poster.empty() || t->rec.users.empty()) {
                        continue;
                    }
                    makeDirs(coverPath(m_store.dir(), hash).substr(0, coverPath(m_store.dir(), hash).rfind('/')));
                    const json meta = {{"source", source}, {"type", type},
                                       {"bytes", static_cast<int64_t>(bytes.size())},
                                       {"at", static_cast<int64_t>(time(nullptr))}};
                    if (writeWhole(base + ".img", bytes) && writeWhole(base + ".json", meta.dump())) {
                        std::remove((base + ".none").c_str());
                        log("[Cover:%s] %s, %s, %d bytes", hash.c_str(), source.c_str(), type.c_str(),
                            static_cast<int>(bytes.size()));
                        error.clear();
                    } else {
                        error = "the cover cache is not writable";
                        std::remove((base + ".img").c_str());
                        std::remove((base + ".json").c_str());
                    }
                }
                if (!error.empty() && error != "torrent metadata is not ready") {
                    makeDirs(coverPath(m_store.dir(), hash).substr(0, coverPath(m_store.dir(), hash).rfind('/')));
                    writeWhole(base + ".none",
                               json{{"reason", error}, {"at", static_cast<int64_t>(time(nullptr))}}.dump());
                    log("[Cover:%s] none: %s", hash.c_str(), error.c_str());
                } else if (error == "torrent metadata is not ready") {
                    log("[Cover:%s] metadata is not ready; will retry when it arrives", hash.c_str());
                }
            } catch (const std::exception &e) {
                log("[Cover:%s] failed: %s", hash.c_str(), e.what());
            }
        }
    }

}
