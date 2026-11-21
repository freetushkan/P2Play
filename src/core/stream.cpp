// Streaming a file of a torrent: HTTP Range over the pieces, downloaded around each
// reader (its window) and read from the RAM storage, or through read_piece on disk.

#include <algorithm>
#include <cctype>

#include <libtorrent/torrent_info.hpp>

#include "core_torrent.hpp"

namespace p2p {

    using Clock = std::chrono::steady_clock;

    namespace {
        std::string mimeType(const std::string &name) {
            auto dot = name.rfind('.');
            std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
            if (ext == "mkv") return "video/x-matroska";
            if (ext == "mp4" || ext == "m4v") return "video/mp4";
            if (ext == "avi") return "video/x-msvideo";
            if (ext == "ts" || ext == "m2ts") return "video/mp2t";
            if (ext == "webm") return "video/webm";
            if (ext == "mov") return "video/quicktime";
            if (ext == "wmv") return "video/x-ms-wmv";
            if (ext == "mpg" || ext == "mpeg") return "video/mpeg";
            if (ext == "mp3") return "audio/mpeg";
            if (ext == "flac") return "audio/flac";
            if (ext == "m4a" || ext == "aac") return "audio/mp4";
            if (ext == "ogg" || ext == "opus") return "audio/ogg";
            if (ext == "wav") return "audio/wav";
            if (ext == "srt") return "application/x-subrip";
            if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
            if (ext == "png") return "image/png";
            return "application/octet-stream";
        }
    }

    // the readers' window: at most half of the RAM cache (the other half keeps what was read)
    int64_t Core::windowBytes() {
        int64_t bytes = m_settings.cacheSize * m_settings.readAhead / 100;
        if (m_cache) {
            bytes = std::min<int64_t>(bytes, m_settings.cacheSize / 2);
        }
        return std::max<int64_t>(bytes, 4LL << 20);
    }

    // reader's pieces [first, last] (first < 0: the reader is gone). In the union of the
    // windows and the pinned pieces: top priority and deadlines in reading order; out of it:
    // not downloaded
    void Core::setWindow(const TorrentPtr &t, int reader, int first, int last) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!t->active) {
            return;
        }
        if (first < 0) {
            t->windows.erase(reader);
        } else {
            t->windows[reader] = {first, std::min(last, t->numPieces - 1)};
        }
        if (m_cache) {
            m_cache->setReader(t->cacheKey, reader, first < 0 ? -1 : first, std::min(last, t->numPieces - 1));
        }
        std::set<int> now = t->pinned;
        for (const auto &w : t->windows) {
            for (int p = w.second.first; p <= w.second.second; p++) {
                now.insert(p);
            }
        }
        lt::torrent_handle &h = t->handle;
        std::vector<std::pair<lt::piece_index_t, lt::download_priority_t>> prio;
        for (int p : now) {
            if (!t->wanted.count(p)) {
                prio.emplace_back(lt::piece_index_t{p}, lt::top_priority);
            }
        }
        for (int p : t->wanted) {
            if (!now.count(p)) {
                prio.emplace_back(lt::piece_index_t{p}, lt::dont_download);
                h.reset_piece_deadline(lt::piece_index_t{p});
            }
        }
        if (!prio.empty()) {
            h.prioritize_pieces(prio);
        }
        for (const auto &w : t->windows) {
            int order = 0;
            for (int p = w.second.first; p <= w.second.second; p++, order++) {
                if (!t->wanted.count(p)) {
                    h.set_piece_deadline(lt::piece_index_t{p}, order * 100);
                }
            }
        }
        for (int p : t->pinned) {
            if (!t->wanted.count(p)) {
                h.set_piece_deadline(lt::piece_index_t{p}, 0);
            }
        }
        t->wanted = std::move(now);
    }

    bool Core::waitPiece(const TorrentPtr &t, int piece, std::chrono::seconds timeout) {
        lt::torrent_handle h;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            h = t->handle;
        }
        auto deadline = Clock::now() + timeout;
        while (!h.have_piece(lt::piece_index_t{piece})) {
            if (m_stopping || Clock::now() > deadline || !h.is_valid()) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return true;
    }

    // a downloaded piece: RAM storage directly (a piece dropped meanwhile is downloaded
    // again), disk storage through read_piece (libtorrent 2.0 checks a piece before its
    // write reaches the file)
    std::shared_ptr<const std::vector<char>> Core::readPiece(const TorrentPtr &t, int piece, std::chrono::seconds timeout) {
        auto deadline = Clock::now() + timeout;
        while (!m_stopping && Clock::now() < deadline) {
            if (!waitPiece(t, piece, std::chrono::duration_cast<std::chrono::seconds>(deadline - Clock::now()) +
                                     std::chrono::seconds(1))) {
                return nullptr;
            }
            if (m_cache) {
                lt::sha1_hash key;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    key = t->cacheKey;
                }
                if (auto data = m_cache->piece(key, piece)) {
                    return data;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            auto key = std::make_pair(t->rec.hash, piece);
            lt::torrent_handle h;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                h = t->handle;
            }
            h.read_piece(lt::piece_index_t{piece});
            std::unique_lock<std::mutex> lock(m_readsMutex);
            auto until = std::min(deadline, Clock::now() + std::chrono::seconds(5));
            while (!m_stopping && Clock::now() < until) {
                auto it = m_reads.find(key);
                if (it != m_reads.end()) {
                    auto data = it->second;
                    m_reads.erase(it);
                    return data;
                }
                if (m_readsFailed.erase(key)) {
                    break;
                }
                lock.unlock();
                {
                    std::unique_lock<std::mutex> wait(m_mutex);
                    m_changed.wait_for(wait, std::chrono::milliseconds(50));
                }
                lock.lock();
            }
        }
        return nullptr;
    }

    void Core::stream(const TorrentPtr &t, int index, const HttpRequest &req, HttpResponse &res) {
        std::string name;
        int64_t size = 0, offset = 0;
        int pieceLength = 0, firstPiece = 0, lastPiece = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!t->active || !t->gotInfo || index < 1 || index > static_cast<int>(t->fileIndex.size())) {
                res.text(404, "no such file\n");
                return;
            }
            std::shared_ptr<const lt::torrent_info> ti = t->handle.torrent_file();
            if (!ti) {
                res.text(503, "no metadata\n");
                return;
            }
            lt::file_index_t fi = t->fileIndex[static_cast<size_t>(index - 1)];
            name = std::string(ti->files().file_name(fi));
            size = ti->files().file_size(fi);
            offset = ti->files().file_offset(fi);
            pieceLength = t->pieceLength;
            firstPiece = static_cast<int>(offset / pieceLength);
            lastPiece = static_cast<int>((offset + std::max<int64_t>(size, 1) - 1) / pieceLength);
            t->readers++;
            t->lastUse = Clock::now();
            // container headers and indexes at both ends
            t->pinned.insert(firstPiece);
            t->pinned.insert(lastPiece);
        }
        const int reader = m_nextReader++;
        auto done = [&] {
            setWindow(t, reader, -1, -1);
            std::lock_guard<std::mutex> lock(m_mutex);
            t->readers--;
            t->lastUse = Clock::now();
        };

        int64_t start = 0, end = size - 1;
        if (req.hasRange) {
            if (req.rangeStart < 0) {
                start = std::max<int64_t>(0, size + req.rangeStart);
            } else {
                start = req.rangeStart;
                if (req.rangeEnd >= 0) {
                    end = std::min(req.rangeEnd, size - 1);
                }
            }
        }
        if (size == 0 || start >= size || start > end) {
            res.begin(416, "", 0, {{"Content-Range", "bytes */" + std::to_string(size)}});
            done();
            return;
        }
        std::map<std::string, std::string> headers = {{"Accept-Ranges", "bytes"}};
        if (req.hasRange) {
            headers["Content-Range"] = "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(size);
        }
        if (!res.begin(req.hasRange ? 206 : 200, mimeType(name), end - start + 1, headers) || req.method == "HEAD") {
            done();
            return;
        }

        const int ahead = std::max<int>(2, static_cast<int>(windowBytes() / pieceLength));
        int64_t pos = start, sent = 0;
        auto began = Clock::now();
        while (pos <= end && !m_stopping) {
            int piece = static_cast<int>((offset + pos) / pieceLength);
            setWindow(t, reader, piece, std::min(lastPiece, piece + ahead - 1));
            auto data = readPiece(t, piece, std::chrono::minutes(3));
            if (!data) {
                log("[Stream:%s] piece %d not received", t->rec.hash.c_str(), piece);
                break;
            }
            int64_t pieceStart = static_cast<int64_t>(piece) * pieceLength - offset;
            int64_t from = pos - pieceStart;
            int64_t to = std::min<int64_t>(static_cast<int64_t>(data->size()), end + 1 - pieceStart);
            if (from >= to || !res.write(data->data() + from, static_cast<size_t>(to - from))) {
                break;
            }
            pos += to - from;
            sent += to - from;
            std::lock_guard<std::mutex> lock(m_mutex);
            t->lastUse = Clock::now();
        }
        done();
        log("[Stream:%s] %s %d: %lld-%lld, %.1f MB in %.0f s", t->rec.hash.c_str(), req.method.c_str(), index,
            static_cast<long long>(start), static_cast<long long>(end), static_cast<double>(sent) / 1048576.0,
            std::chrono::duration<double>(Clock::now() - began).count());
    }

    // TorrServer's preload: the start of the file (PreloadCache % of the cache, at most the
    // window) and its last piece, before the player opens it
    void Core::preload(const TorrentPtr &t, int index) {
        int first, last, tail, pieceLength;
        int64_t bytes = std::min<int64_t>(m_settings.cacheSize * m_settings.preloadCache / 100, windowBytes());
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!t->active || !t->gotInfo || index < 1 || index > static_cast<int>(t->fileIndex.size()) ||
                t->preloading) {
                return;
            }
            std::shared_ptr<const lt::torrent_info> ti = t->handle.torrent_file();
            lt::file_index_t fi = t->fileIndex[static_cast<size_t>(index - 1)];
            int64_t offset = ti->files().file_offset(fi), size = ti->files().file_size(fi);
            pieceLength = t->pieceLength;
            bytes = std::min(bytes, size);
            first = static_cast<int>(offset / pieceLength);
            last = static_cast<int>((offset + std::max<int64_t>(bytes, 1) - 1) / pieceLength);
            tail = static_cast<int>((offset + std::max<int64_t>(size, 1) - 1) / pieceLength);
            t->pinned.insert(tail);
            t->preloading = true;
            t->preloadSize = bytes;
            t->preloadedBytes = 0;
        }
        const int reader = m_nextReader++;
        setWindow(t, reader, first, last);
        lt::torrent_handle h;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            h = t->handle;
        }
        auto deadline = Clock::now() + std::chrono::minutes(2);
        while (!m_stopping && Clock::now() < deadline && h.is_valid()) {
            int have = 0;
            for (int p = first; p <= last; p++) {
                have += h.have_piece(lt::piece_index_t{p}) ? 1 : 0;
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                t->preloadedBytes = std::min<int64_t>(t->preloadSize, static_cast<int64_t>(have) * pieceLength);
                t->lastUse = Clock::now();
            }
            if (have == last - first + 1) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        setWindow(t, reader, -1, -1);
        std::lock_guard<std::mutex> lock(m_mutex);
        t->preloading = false;
    }

}
