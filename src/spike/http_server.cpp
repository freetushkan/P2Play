#include "http_server.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

using namespace p2p;

namespace {
    bool parseRange(const std::string &value, HttpRequest &request) {
        const std::string prefix = "bytes=";
        if (value.compare(0, prefix.size(), prefix) != 0) {
            return false;
        }
        std::string spec = value.substr(prefix.size());
        size_t dash = spec.find('-');
        if (dash == std::string::npos || dash == 0) {
            return false;
        }
        request.rangeStart = strtoll(spec.substr(0, dash).c_str(), nullptr, 10);
        std::string end = spec.substr(dash + 1);
        request.rangeEnd = end.empty() ? -1 : strtoll(end.c_str(), nullptr, 10);
        request.hasRange = true;
        return true;
    }

    bool parseRequest(const std::string &head, HttpRequest &request) {
        size_t lineEnd = head.find("\r\n");
        std::string line = head.substr(0, lineEnd);
        size_t sp1 = line.find(' ');
        size_t sp2 = line.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) {
            return false;
        }
        request.method = line.substr(0, sp1);
        request.path = line.substr(sp1 + 1, sp2 - sp1 - 1);

        size_t pos = lineEnd + 2;
        while (pos < head.size()) {
            size_t end = head.find("\r\n", pos);
            if (end == std::string::npos || end == pos) {
                break;
            }
            std::string header = head.substr(pos, end - pos);
            size_t colon = header.find(':');
            if (colon != std::string::npos && strncasecmp(header.c_str(), "range", colon) == 0 && colon == 5) {
                std::string value = header.substr(colon + 1);
                value.erase(0, value.find_first_not_of(' '));
                parseRange(value, request);
            }
            pos = end + 2;
        }
        return true;
    }
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
    if (bind(m_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(m_fd, 8) != 0) {
        close(m_fd);
        m_fd = -1;
        return false;
    }
    m_running = true;
    m_thread = std::thread(&HttpServer::acceptLoop, this);
    return true;
}

void HttpServer::stop() {
    if (!m_running.exchange(false)) {
        return;
    }
    shutdown(m_fd, SHUT_RDWR);
    close(m_fd);
    m_fd = -1;
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void HttpServer::acceptLoop() {
    while (m_running) {
        int client = accept(m_fd, nullptr, nullptr);
        if (client < 0) {
            if (!m_running) {
                break;
            }
            continue;
        }
        std::thread(&HttpServer::serve, this, client).detach();
    }
}

void HttpServer::serve(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    std::string head;
    char buf[2048];
    while (head.find("\r\n\r\n") == std::string::npos && head.size() < 16384) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            close(fd);
            return;
        }
        head.append(buf, static_cast<size_t>(n));
    }

    HttpRequest request;
    if (!parseRequest(head, request)) {
        sendText(fd, 400, "text/plain", "bad request\n");
    } else {
        m_handler(fd, request);
    }
    shutdown(fd, SHUT_RDWR);
    close(fd);
}

bool HttpServer::sendAll(int fd, const void *data, size_t size) {
    const char *p = static_cast<const char *>(data);
    while (size > 0) {
        ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
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

bool HttpServer::sendText(int fd, int status, const std::string &type, const std::string &body) {
    const char *reason = status == 200 ? "OK" : status == 404 ? "Not Found" : status == 503 ? "Service Unavailable" : "Bad Request";
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
                      "Content-Type: " + type + "\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n"
                      "Connection: close\r\n\r\n" + body;
    return sendAll(fd, out.data(), out.size());
}
