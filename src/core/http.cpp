#include "http.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace p2p {

    namespace {
        constexpr size_t kMaxHead = 32 * 1024;
        constexpr size_t kMaxBody = 16 * 1024 * 1024;  // a .torrent upload

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        std::string trim(const std::string &s) {
            size_t a = s.find_first_not_of(" \t");
            size_t b = s.find_last_not_of(" \t");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        }

        // "bytes=a-b", "bytes=a-", "bytes=-n" (the last n bytes)
        void parseRange(const std::string &value, HttpRequest &r) {
            const std::string prefix = "bytes=";
            if (value.compare(0, prefix.size(), prefix) != 0) {
                return;
            }
            std::string spec = value.substr(prefix.size());
            spec = spec.substr(0, spec.find(','));
            size_t dash = spec.find('-');
            if (dash == std::string::npos) {
                return;
            }
            std::string a = trim(spec.substr(0, dash)), b = trim(spec.substr(dash + 1));
            if (a.empty() && b.empty()) {
                return;
            }
            if (a.empty()) {
                r.rangeStart = -strtoll(b.c_str(), nullptr, 10);  // resolved by the handler
                r.rangeEnd = -1;
            } else {
                r.rangeStart = strtoll(a.c_str(), nullptr, 10);
                r.rangeEnd = b.empty() ? -1 : strtoll(b.c_str(), nullptr, 10);
            }
            r.hasRange = true;
        }

        void parseQuery(const std::string &q, HttpRequest &r) {
            size_t pos = 0;
            while (pos <= q.size()) {
                size_t amp = q.find('&', pos);
                std::string item = q.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
                if (!item.empty()) {
                    size_t eq = item.find('=');
                    std::string key = HttpServer::urlDecode(item.substr(0, eq), true);
                    std::string value = eq == std::string::npos ? "" : HttpServer::urlDecode(item.substr(eq + 1), true);
                    r.query.emplace(key, value);
                }
                if (amp == std::string::npos) {
                    break;
                }
                pos = amp + 1;
            }
        }

        bool parseHead(const std::string &head, HttpRequest &r) {
            size_t lineEnd = head.find("\r\n");
            std::string line = head.substr(0, lineEnd);
            size_t sp1 = line.find(' ');
            size_t sp2 = line.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) {
                return false;
            }
            r.method = line.substr(0, sp1);
            r.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
            size_t q = r.target.find('?');
            r.path = HttpServer::urlDecode(r.target.substr(0, q), false);
            if (q != std::string::npos) {
                parseQuery(r.target.substr(q + 1), r);
            }
            size_t pos = lineEnd + 2;
            while (pos < head.size()) {
                size_t end = head.find("\r\n", pos);
                if (end == std::string::npos || end == pos) {
                    break;
                }
                std::string h = head.substr(pos, end - pos);
                size_t colon = h.find(':');
                if (colon != std::string::npos) {
                    r.headers[lower(trim(h.substr(0, colon)))] = trim(h.substr(colon + 1));
                }
                pos = end + 2;
            }
            auto range = r.headers.find("range");
            if (range != r.headers.end()) {
                parseRange(range->second, r);
            }
            return true;
        }

        bool sendAll(int fd, const void *data, size_t size) {
            const char *p = static_cast<const char *>(data);
            while (size > 0) {
                ssize_t n = ::send(fd, p, size, MSG_NOSIGNAL);
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                if (n <= 0) {
                    return false;
                }
                p += n;
                size -= static_cast<size_t>(n);
            }
            return true;
        }
    }

    const char *httpReason(int status) {
        switch (status) {
            case 200: return "OK";
            case 204: return "No Content";
            case 206: return "Partial Content";
            case 301: return "Moved Permanently";
            case 302: return "Found";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 400: return "Bad Request";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 413: return "Payload Too Large";
            case 416: return "Range Not Satisfiable";
            case 500: return "Internal Server Error";
            case 503: return "Service Unavailable";
            case 504: return "Gateway Timeout";
            default: return "OK";
        }
    }

    bool HttpResponse::begin(int status, const std::string &type, int64_t length,
                             const std::map<std::string, std::string> &extra) {
        std::string head = "HTTP/1.1 " + std::to_string(status) + " " + httpReason(status) + "\r\n";
        if (!type.empty()) {
            head += "Content-Type: " + type + "\r\n";
        }
        if (length >= 0) {
            head += "Content-Length: " + std::to_string(length) + "\r\n";
        }
        head += "Access-Control-Allow-Origin: *\r\n";
        for (const auto &h : extra) {
            head += h.first + ": " + h.second + "\r\n";
        }
        head += "Connection: close\r\n\r\n";
        m_started = true;
        return sendAll(m_fd, head.data(), head.size());
    }

    bool HttpResponse::write(const void *data, size_t size) {
        return sendAll(m_fd, data, size);
    }

    bool HttpResponse::send(int status, const std::string &type, const std::string &body,
                            const std::map<std::string, std::string> &extra) {
        return begin(status, type, static_cast<int64_t>(body.size()), extra) && write(body.data(), body.size());
    }

    HttpServer::~HttpServer() {
        stop();
    }

    bool HttpServer::start(uint16_t port, Handler handler) {
        m_handler = std::move(handler);
        m_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd < 0) {
            return false;
        }
        int one = 1;
        setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(m_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(m_fd, 16) != 0) {
            close(m_fd);
            m_fd = -1;
            return false;
        }
        socklen_t len = sizeof(addr);
        if (getsockname(m_fd, reinterpret_cast<sockaddr *>(&addr), &len) == 0) {
            m_port = ntohs(addr.sin_port);
        }
        m_running = true;
        m_thread = std::thread(&HttpServer::acceptLoop, this);
        return true;
    }

    void HttpServer::stopAccepting() {
        if (m_running.exchange(false)) {
            if (m_fd >= 0) {
                shutdown(m_fd, SHUT_RDWR);
                close(m_fd);
                m_fd = -1;
            }
        }
        // Join the accept loop before reporting the connection count: a successful accept
        // is counted and its worker is started before this join can complete.
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    void HttpServer::stop() {
        stopAccepting();
        // Connection workers are detached, but each decrements this counter as its last
        // access to HttpServer. Never destroy the handler/core while one is still running.
        while (m_connections.load() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        m_handler = {};
        m_port = 0;
    }

    void HttpServer::acceptLoop() {
        while (m_running) {
            sockaddr_in addr{};
            socklen_t len = sizeof(addr);
            int client = accept(m_fd, reinterpret_cast<sockaddr *>(&addr), &len);
            if (client < 0) {
                if (!m_running) {
                    break;
                }
                continue;
            }
            char ip[INET_ADDRSTRLEN] = "";
            inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
            m_connections++;
            std::thread(&HttpServer::serve, this, client, std::string(ip)).detach();
        }
    }

    void HttpServer::serve(int fd, std::string peer) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        std::string data;
        char buf[8192];
        size_t headEnd;
        while ((headEnd = data.find("\r\n\r\n")) == std::string::npos && data.size() < kMaxHead) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                close(fd);
                m_connections--;
                return;
            }
            data.append(buf, static_cast<size_t>(n));
        }

        HttpRequest request;
        request.peer = peer;
        HttpResponse response(fd);
        if (headEnd == std::string::npos || !parseHead(data.substr(0, headEnd + 2), request)) {
            response.text(400, "bad request\n");
        } else {
            size_t length = static_cast<size_t>(strtoull(request.header("content-length").c_str(), nullptr, 10));
            if (length > kMaxBody) {
                response.text(413, "too large\n");
            } else {
                request.body = data.substr(headEnd + 4);
                while (request.body.size() < length) {
                    ssize_t n = recv(fd, buf, sizeof(buf), 0);
                    if (n <= 0) {
                        break;
                    }
                    request.body.append(buf, static_cast<size_t>(n));
                }
                request.body.resize(std::min(request.body.size(), length));
                if (request.method == "OPTIONS") {
                    // CORS preflight
                    response.begin(204, "", 0, {
                            {"Access-Control-Allow-Methods", "GET, POST, HEAD, OPTIONS"},
                            {"Access-Control-Allow-Headers", "*"},
                            {"Access-Control-Max-Age", "86400"}});
                } else {
                    m_handler(request, response);
                    if (!response.started()) {
                        response.text(500, "no response\n");
                    }
                }
            }
        }
        shutdown(fd, SHUT_RDWR);
        close(fd);
        m_connections--;
    }

    std::string HttpServer::urlDecode(const std::string &s, bool plusIsSpace) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
                out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
                i += 2;
            } else if (plusIsSpace && s[i] == '+') {
                out += ' ';
            } else {
                out += s[i];
            }
        }
        return out;
    }

    std::string HttpServer::urlEncode(const std::string &s) {
        static const char *hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out += static_cast<char>(c);
            } else {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 15];
            }
        }
        return out;
    }

    // multipart/form-data: name -> (filename, content)
    std::map<std::string, std::pair<std::string, std::string>> parseMultipart(const HttpRequest &req) {
        std::map<std::string, std::pair<std::string, std::string>> parts;
        std::string type = req.header("content-type");
        size_t b = type.find("boundary=");
        if (b == std::string::npos) {
            return parts;
        }
        std::string boundary = type.substr(b + 9);
        if (!boundary.empty() && boundary[0] == '"') {
            boundary = boundary.substr(1, boundary.find('"', 1) - 1);
        }
        boundary = "--" + boundary.substr(0, boundary.find(';'));
        const std::string &body = req.body;
        size_t pos = body.find(boundary);
        while (pos != std::string::npos) {
            pos += boundary.size();
            if (body.compare(pos, 2, "--") == 0) {
                break;
            }
            size_t headEnd = body.find("\r\n\r\n", pos);
            if (headEnd == std::string::npos) {
                break;
            }
            std::string head = body.substr(pos, headEnd - pos);
            size_t next = body.find("\r\n" + boundary, headEnd + 4);
            if (next == std::string::npos) {
                break;
            }
            std::string content = body.substr(headEnd + 4, next - headEnd - 4);
            auto attr = [&](const std::string &key) {
                size_t k = head.find(key + "=\"");
                if (k == std::string::npos) {
                    return std::string();
                }
                k += key.size() + 2;
                return head.substr(k, head.find('"', k) - k);
            };
            std::string name = attr("name");
            if (!name.empty() && !parts.count(name)) {
                parts[name] = {attr("filename"), content};
            }
            pos = next + 2;
        }
        return parts;
    }

}
