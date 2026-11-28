#include "https.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#else
#include <curl/curl.h>
#endif

#if defined(__SWITCH__)
#include <switch.h>
#endif

#if defined(P2P_HAS_EMBEDDED_CA)
#include "https_ca_bundle.hpp"
#endif

namespace p2p {
namespace {
    p2play_log_fn gLog = nullptr;
    std::string gCaPath;
    bool gReady = false;

#if defined(__ANDROID__)
    int gBridgePort = 0;
    std::string gBridgeToken;
#else
    std::once_flag gCurlInit;
    CURLcode gCurlInitResult = CURLE_FAILED_INIT;
#endif

    void logMessage(const std::string &message) {
        if (gLog != nullptr) {
            gLog(message.c_str(), nullptr);
        }
    }

    void copyString(char *out, size_t capacity, const std::string &value) {
        if (out == nullptr || capacity == 0) return;
        const size_t count = std::min(capacity - 1, value.size());
        if (count != 0) std::memcpy(out, value.data(), count);
        out[count] = '\0';
    }

    bool writeFile(const std::string &path, const std::string &data) {
        std::FILE *file = std::fopen(path.c_str(), "wb");
        if (file == nullptr) return false;
        const bool ok = std::fwrite(data.data(), 1, data.size(), file) == data.size();
        const bool closed = std::fclose(file) == 0;
        return ok && closed;
    }

#if defined(P2P_HAS_EMBEDDED_CA)
    bool writeEmbeddedCa(const std::string &path) {
        const std::string pem(kEmbeddedCaBundle);
        if (!writeFile(path, pem)) {
            logMessage("[HTTPS] cannot write bundled CA certificates to " + path);
            return false;
        }
        gCaPath = path;
        return true;
    }
#endif

#if defined(__SWITCH__)
    bool writeSwitchCa(const std::string &path) {
        if (R_FAILED(sslInitialize(1))) {
            logMessage("[HTTPS] libnx ssl service is unavailable");
            return false;
        }

        u32 all = static_cast<u32>(SslCaCertificateId_All);
        u32 bufferSize = 0;
        u32 count = 0;
        std::vector<u8> buffer;
        if (R_SUCCEEDED(sslGetCertificateBufSize(&all, 1, &bufferSize)) && bufferSize > 0) {
            buffer.resize(bufferSize);
            if (R_FAILED(sslGetCertificates(buffer.data(), bufferSize, &all, 1, &count))) {
                count = 0;
            }
        }
        sslExit();
        if (count == 0 || count > 2048) {
            logMessage("[HTTPS] libnx returned no trusted CA certificates");
            return false;
        }

        static constexpr char alphabet[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string pem;
        const auto *infos = reinterpret_cast<const SslBuiltInCertificateInfo *>(buffer.data());
        size_t written = 0;
        for (u32 i = 0; i < count; ++i) {
            if (infos[i].status != SslTrustedCertStatus_EnabledTrusted || infos[i].cert_data == nullptr ||
                infos[i].cert_size == 0 || infos[i].cert_size > (1u << 20)) {
                continue;
            }
            const u8 *data = infos[i].cert_data;
            std::string encoded;
            encoded.reserve(static_cast<size_t>((infos[i].cert_size + 2) / 3) * 4);
            for (u64 j = 0; j < infos[i].cert_size; j += 3) {
                const u32 a = data[j];
                const u32 b = j + 1 < infos[i].cert_size ? data[j + 1] : 0;
                const u32 c = j + 2 < infos[i].cert_size ? data[j + 2] : 0;
                const u32 triple = (a << 16) | (b << 8) | c;
                encoded += alphabet[(triple >> 18) & 63];
                encoded += alphabet[(triple >> 12) & 63];
                encoded += j + 1 < infos[i].cert_size ? alphabet[(triple >> 6) & 63] : '=';
                encoded += j + 2 < infos[i].cert_size ? alphabet[triple & 63] : '=';
            }
            pem += "-----BEGIN CERTIFICATE-----\n";
            for (size_t offset = 0; offset < encoded.size(); offset += 64) {
                pem += encoded.substr(offset, 64) + "\n";
            }
            pem += "-----END CERTIFICATE-----\n";
            ++written;
        }
        if (written == 0 || !writeFile(path, pem)) {
            logMessage("[HTTPS] cannot persist libnx trusted CA certificates");
            return false;
        }
        gCaPath = path;
        logMessage("[HTTPS] exported " + std::to_string(written) + " trusted CA certificates from libnx");
        return true;
    }
#endif

#if defined(__ANDROID__)
    bool sendAll(int socketFd, const void *data, size_t size) {
        const char *p = static_cast<const char *>(data);
        size_t sent = 0;
        while (sent < size) {
            const ssize_t count = ::send(socketFd, p + sent, size - sent, MSG_NOSIGNAL);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
            sent += static_cast<size_t>(count);
        }
        return true;
    }

    bool receiveAll(int socketFd, void *data, size_t size) {
        char *p = static_cast<char *>(data);
        size_t received = 0;
        while (received < size) {
            const ssize_t count = ::recv(socketFd, p + received, size - received, 0);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
            received += static_cast<size_t>(count);
        }
        return true;
    }

    bool sendU16(int fd, uint16_t value) {
        const unsigned char b[2] = {static_cast<unsigned char>(value >> 8), static_cast<unsigned char>(value)};
        return sendAll(fd, b, sizeof(b));
    }
    bool sendU32(int fd, uint32_t value) {
        const unsigned char b[4] = {static_cast<unsigned char>(value >> 24),
                                    static_cast<unsigned char>(value >> 16),
                                    static_cast<unsigned char>(value >> 8),
                                    static_cast<unsigned char>(value)};
        return sendAll(fd, b, sizeof(b));
    }
    bool receiveU16(int fd, uint16_t *value) {
        unsigned char b[2]{};
        if (!receiveAll(fd, b, sizeof(b))) return false;
        *value = static_cast<uint16_t>((static_cast<uint16_t>(b[0]) << 8) | b[1]);
        return true;
    }
    bool receiveU32(int fd, uint32_t *value) {
        unsigned char b[4]{};
        if (!receiveAll(fd, b, sizeof(b))) return false;
        *value = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
                 (static_cast<uint32_t>(b[2]) << 8) | b[3];
        return true;
    }
#else
    struct CurlContext {
        char *body = nullptr;
        size_t capacity = 0;
        size_t size = 0;
        bool overflow = false;
        std::string contentType;
        std::string location;
    };

    size_t writeBody(char *data, size_t size, size_t count, void *opaque) {
        auto *ctx = static_cast<CurlContext *>(opaque);
        const size_t length = size * count;
        if (length > ctx->capacity - std::min(ctx->capacity, ctx->size)) {
            ctx->overflow = true;
            return 0;
        }
        if (length != 0) std::memcpy(ctx->body + ctx->size, data, length);
        ctx->size += length;
        return length;
    }

    std::string trimHeader(const char *data, size_t length) {
        std::string value(data, length);
        const size_t first = value.find_first_not_of(" \t");
        if (first == std::string::npos) return {};
        const size_t last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    std::string lowerAscii(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

    size_t readHeader(char *data, size_t size, size_t count, void *opaque) {
        auto *ctx = static_cast<CurlContext *>(opaque);
        const size_t length = size * count;
        const std::string line(data, length);
        const size_t colon = line.find(':');
        if (colon == std::string::npos) return length;
        const std::string name = lowerAscii(line.substr(0, colon));
        const std::string value = trimHeader(data + colon + 1, length - colon - 1);
        if (name == "location") ctx->location = value;
        else if (name == "content-type") ctx->contentType = value;
        return length;
    }
#endif
} // namespace

bool httpsInit(const std::string &dataDir, int androidBridgePort, const std::string &androidBridgeToken, p2play_log_fn log) {
    gLog = log;
#if defined(__ANDROID__)
    (void) dataDir;
    gBridgePort = androidBridgePort;
    gBridgeToken = androidBridgeToken;
    gReady = gBridgePort > 0 && gBridgePort <= 65535 && gBridgeToken.size() == 32 &&
             std::all_of(gBridgeToken.begin(), gBridgeToken.end(), [](unsigned char ch) {
                 return std::isxdigit(ch) != 0;
             });
    if (gReady) {
        logMessage("[HTTPS] Android system TLS bridge listening on authenticated loopback port " + std::to_string(gBridgePort));
    } else {
        logMessage("[HTTPS] Android TLS bridge unavailable; HTTPS poster fetching is disabled");
    }
    return gReady;
#else
    std::call_once(gCurlInit, [] { gCurlInitResult = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (gCurlInitResult != CURLE_OK) {
        logMessage(std::string("[HTTPS] curl global init failed: ") + curl_easy_strerror(gCurlInitResult));
        return false;
    }
    const curl_version_info_data *curlInfo = curl_version_info(CURLVERSION_NOW);
    if (curlInfo == nullptr || curlInfo->ssl_version == nullptr || curlInfo->ssl_version[0] == '\0') {
        logMessage("[HTTPS] this libcurl build has no TLS backend");
        return false;
    }

#if defined(__PS4__) || defined(__PS5__)
#if defined(P2P_HAS_EMBEDDED_CA)
    if (!writeEmbeddedCa(dataDir + "/cacert.pem")) return false;
#else
    logMessage("[HTTPS] this console build has no bundled CA certificate set");
    return false;
#endif
#elif defined(__SWITCH__)
    // Use the device's current trusted CA list, not a stale bundled snapshot.
    if (!writeSwitchCa(dataDir + "/system_ca.pem")) return false;
#elif defined(__ANDROID__)
    (void) dataDir;
#else
    // Linux deliberately leaves CURLOPT_CAINFO unset: libcurl uses the distribution's
    // system CA bundle (including administrator trust-store updates).
    gCaPath.clear();
    (void) dataDir;
#endif
    gReady = true;
    logMessage("[HTTPS] libcurl TLS transport ready; certificate and hostname verification are enabled");
    return true;
#endif
}

void httpsShutdown() {
#if defined(__ANDROID__)
    gBridgePort = 0;
    gBridgeToken.clear();
#endif
    gReady = false;
}

int httpsGet(const char *url, const char *pinnedResolve,
             char *body, size_t bodyCapacity, size_t *bodySize,
             char *contentType, size_t contentTypeCapacity,
             char *location, size_t locationCapacity,
             char *error, size_t errorCapacity, void *) {
    if (bodySize != nullptr) *bodySize = 0;
    if (contentType != nullptr && contentTypeCapacity) contentType[0] = '\0';
    if (location != nullptr && locationCapacity) location[0] = '\0';
    if (error != nullptr && errorCapacity) error[0] = '\0';

    if (!gReady || url == nullptr || pinnedResolve == nullptr || body == nullptr || bodyCapacity == 0) {
        copyString(error, errorCapacity, "HTTPS transport is not initialised or its request is invalid");
        return 0;
    }
    const std::string requestUrl(url);
    const std::string resolve(pinnedResolve);
    const bool https = requestUrl.rfind("https://", 0) == 0;
    const bool http = requestUrl.rfind("http://", 0) == 0;
    if ((!https && !http) || requestUrl.size() > 2048 ||
        resolve.empty() || resolve.size() > 512 || requestUrl.find_first_of("\r\n") != std::string::npos ||
        resolve.find_first_of("\r\n") != std::string::npos) {
        copyString(error, errorCapacity, "HTTP(S) URL or pinned DNS entry is invalid");
        return 0;
    }

#if defined(__ANDROID__)
    if (gBridgePort <= 0) {
        copyString(error, errorCapacity, "Android system TLS bridge is unavailable");
        return 0;
    }
    const size_t urlLength = requestUrl.size();
    const size_t resolveLength = resolve.size();
    if (urlLength > 2048 || resolveLength > 512 || bodyCapacity > UINT32_MAX) {
        copyString(error, errorCapacity, "HTTPS request exceeds bridge limits");
        return 0;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        copyString(error, errorCapacity, "cannot create loopback HTTPS bridge socket");
        return 0;
    }
    timeval timeout{25, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(gBridgePort));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        ::close(fd);
        copyString(error, errorCapacity, "cannot connect to Android loopback HTTPS bridge");
        return 0;
    }

    static constexpr char requestMagic[] = "P2PTLS01";
    bool ok = sendAll(fd, requestMagic, 8) && sendAll(fd, gBridgeToken.data(), gBridgeToken.size()) &&
              sendU16(fd, static_cast<uint16_t>(urlLength)) &&
              sendU16(fd, static_cast<uint16_t>(resolveLength)) &&
              sendU32(fd, static_cast<uint32_t>(bodyCapacity)) &&
              sendAll(fd, requestUrl.data(), urlLength) && sendAll(fd, resolve.data(), resolveLength);
    char responseMagic[8]{};
    uint32_t status = 0, bodyLength = 0;
    uint16_t typeLength = 0, locationLength = 0, errorLength = 0;
    static constexpr char expectedMagic[] = "P2PTLR01";
    if (ok) {
        ok = receiveAll(fd, responseMagic, sizeof(responseMagic)) &&
             std::memcmp(responseMagic, expectedMagic, sizeof(responseMagic)) == 0 &&
             receiveU32(fd, &status) && receiveU32(fd, &bodyLength) &&
             receiveU16(fd, &typeLength) && receiveU16(fd, &locationLength) && receiveU16(fd, &errorLength);
    }
    if (ok && (bodyLength > bodyCapacity || typeLength > 8192 || locationLength > 8192 || errorLength > 8192)) {
        ok = false;
    }

    std::string responseType, redirect, failure;
    if (ok) {
        responseType.resize(typeLength);
        redirect.resize(locationLength);
        failure.resize(errorLength);
        ok = receiveAll(fd, responseType.data(), responseType.size()) &&
             receiveAll(fd, redirect.data(), redirect.size()) &&
             receiveAll(fd, failure.data(), failure.size()) &&
             receiveAll(fd, body, bodyLength);
    }
    ::close(fd);
    if (!ok) {
        copyString(error, errorCapacity, "invalid or incomplete response from Android HTTPS bridge");
        return 0;
    }
    if (bodySize != nullptr) *bodySize = bodyLength;
    copyString(contentType, contentTypeCapacity, responseType);
    copyString(location, locationCapacity, redirect);
    copyString(error, errorCapacity, failure);
    return static_cast<int>(status);
#else
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        copyString(error, errorCapacity, "curl_easy_init failed");
        return 0;
    }

    CurlContext context{};
    context.body = body;
    context.capacity = bodyCapacity;
    char errorBuffer[CURL_ERROR_SIZE]{};
    curl_slist *resolveList = nullptr;
    curl_slist *headers = nullptr;
    resolveList = curl_slist_append(resolveList, resolve.c_str());
    headers = curl_slist_append(headers, "Accept: */*");
    curl_easy_setopt(curl, CURLOPT_URL, requestUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, readHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &context);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_RESOLVE, resolveList);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "P2Play/0.1");
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    if (!gCaPath.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, gCaPath.c_str());
#ifdef P2P_HTTPS_TEST
    if (const char *testCa = std::getenv("P2PLAY_HTTPS_TEST_CA")) {
        if (testCa[0] != '\0') curl_easy_setopt(curl, CURLOPT_CAINFO, testCa);
    }
#endif

    const CURLcode result = curl_easy_perform(curl);
    long responseStatus = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseStatus);
    char *responseContentType = nullptr;
    if (curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &responseContentType) == CURLE_OK &&
        responseContentType != nullptr && context.contentType.empty()) {
        context.contentType = responseContentType;
    }
    if (bodySize != nullptr) *bodySize = context.size;
    copyString(contentType, contentTypeCapacity, context.contentType);
    copyString(location, locationCapacity, context.location);

    std::string failure;
    if (context.overflow) failure = "HTTP(S) response exceeds the configured size limit";
    else if (result != CURLE_OK) {
        failure = errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(result);
    }
    copyString(error, errorCapacity, failure);

    if (headers != nullptr) curl_slist_free_all(headers);
    if (resolveList != nullptr) curl_slist_free_all(resolveList);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || context.overflow) return 0;
    return static_cast<int>(responseStatus);
#endif
}

} // namespace p2p
