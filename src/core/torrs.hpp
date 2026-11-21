#pragma once

// torrs:// links, TorrServer's TorrsHash (Silo internal/torrshash, TorrServe's TorrsHash.kt):
// base62 of zlib(the 20 bytes of the v1 infohash, then fields: a tag byte; strings with a
// little-endian uint16 length, the size as a little-endian int64). Tags: 0 title, 1 poster,
// 2 tracker (repeated), 3 category, 4 size. Self-contained (an own inflate and deflate): zlib
// is not on every target.

#include <cstdint>
#include <string>
#include <vector>

namespace p2p {

    struct Torrs {
        std::string hash;   // hex, lower case
        std::string title;
        std::string poster;
        std::string category;
        std::vector<std::string> trackers;
        int64_t size = 0;
    };

    // with or without "torrs://"; false: not a valid one
    bool torrsUnpack(const std::string &link, Torrs &out);

    // "torrs://..."
    std::string torrsPack(const Torrs &t);

    // zlib streams (RFC 1950/1951); exposed for the tests
    bool zlibInflate(const std::string &in, std::string &out, size_t limit = 1 << 20);

    std::string zlibDeflate(const std::string &in);

}
