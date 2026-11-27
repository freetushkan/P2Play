#pragma once

// A small HTTP/1.1 server: blocking sockets, a thread per connection, one request per
// connection. Enough for a few players and a web UI; every response allows any origin
// (Lampa calls the server from a browser page).

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace p2p {

    struct HttpRequest {
        std::string method;
        std::string target;  // as sent: path and query
        std::string path;    // decoded, without the query
        std::map<std::string, std::string> query;   // decoded; a key without a value maps to ""
        std::map<std::string, std::string> headers; // lower-case names
        std::string body;
        std::string peer;    // the client's address

        bool hasRange = false;
        int64_t rangeStart = 0;
        int64_t rangeEnd = -1;  // -1: to the end; with rangeSuffix, the suffix length

        bool has(const std::string &key) const { return query.count(key) > 0; }

        std::string param(const std::string &key) const {
            auto it = query.find(key);
            return it == query.end() ? std::string() : it->second;
        }

        std::string header(const std::string &name) const {
            auto it = headers.find(name);
            return it == headers.end() ? std::string() : it->second;
        }
    };

    // the response side of a connection
    class HttpResponse {
    public:
        explicit HttpResponse(int fd) : m_fd(fd) {}

        // the status line and headers; Content-Length when length >= 0
        bool begin(int status, const std::string &type, int64_t length,
                   const std::map<std::string, std::string> &extra = {});

        bool write(const void *data, size_t size);

        bool send(int status, const std::string &type, const std::string &body,
                  const std::map<std::string, std::string> &extra = {});

        bool json(int status, const std::string &body) { return send(status, "application/json; charset=utf-8", body); }

        bool text(int status, const std::string &body) { return send(status, "text/plain; charset=utf-8", body); }

        bool started() const { return m_started; }

    private:
        int m_fd;
        bool m_started = false;
    };

    class HttpServer {
    public:
        using Handler = std::function<void(const HttpRequest &, HttpResponse &)>;

        ~HttpServer();

        // port 0: any free one (port() tells which)
        bool start(uint16_t port, Handler handler);

        // Stop the listener without terminating already accepted connections.
        void stopAccepting();

        // Stop the listener and wait until all accepted connections have returned.
        void stop();

        uint16_t port() const { return m_port; }

        int activeConnections() const { return m_connections.load(); }

        static std::string urlDecode(const std::string &s, bool plusIsSpace);

        static std::string urlEncode(const std::string &s);

    private:
        void acceptLoop();

        void serve(int fd, std::string peer);
        void closeClient(int fd);

        int m_fd = -1;
        uint16_t m_port = 0;
        Handler m_handler;
        std::thread m_thread;
        std::atomic<bool> m_running{false};
        std::atomic<int> m_connections{0};
        std::mutex m_clientsMutex;
        std::set<int> m_clients;
    };

    const char *httpReason(int status);

    // multipart/form-data: name -> (filename, content)
    std::map<std::string, std::pair<std::string, std::string>> parseMultipart(const HttpRequest &req);

}
