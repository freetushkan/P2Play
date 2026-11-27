#pragma once

// Core::Torrent, shared by core.cpp, stream.cpp and api.cpp. Fields under Core::m_mutex.

#include "core.hpp"

namespace p2p {

    struct Core::Torrent {
        TorrentRecord rec;
        bool saved = false;               // in the library (torrents.json)

        bool active = false;              // in the session
        lt::torrent_handle handle;
        bool gotInfo = false;
        bool diskStored = false;           // storage backend chosen when this handle was activated
        std::map<int, lt::file_index_t> fileIndex;  // MatriX's file id -> the torrent's file (no pad files)
        int pieceLength = 0;
        int numPieces = 0;
        lt::sha1_hash cacheKey;           // the RAM storage's key

        int readers = 0;                  // streams being served
        std::chrono::steady_clock::time_point lastUse;
        bool preloading = false;
        int64_t preloadSize = 0;
        int64_t preloadedBytes = 0;

        // reader -> its pieces [first, last]; their union at top priority
        std::map<int, std::pair<int, int>> windows;
        std::set<int> wanted;
        std::set<int> pinned;             // both ends of the files being played (headers, indexes)
    };

}
