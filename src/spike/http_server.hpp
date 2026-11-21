#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace p2p {

    struct HttpRequest {
        std::string method;
        std::string path;
        bool hasRange = false;
        int64_t rangeStart = 0;
        int64_t rangeEnd = -1;
    };

    // blocking sockets, a thread per connection: enough for one player
    class HttpServer {
    public:
        using Handler = std::function<void(int fd, const HttpRequest &request)>;

        ~HttpServer();

        bool start(uint16_t port, Handler handler);

        void stop();

        static bool sendAll(int fd, const void *data, size_t size);

        static bool sendText(int fd, int status, const std::string &type, const std::string &body);

    private:
        void acceptLoop();

        void serve(int fd);

        int m_fd = -1;
        Handler m_handler;
        std::thread m_thread;
        std::atomic<bool> m_running{false};
    };

}
