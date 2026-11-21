#include "torrs.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace p2p {

    namespace {
        const char *kBase62 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

        const short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                    67, 83, 99, 115, 131, 163, 195, 227, 258};
        const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        const short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                     1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
                                      9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

        uint32_t adler32(const std::string &data) {
            uint32_t a = 1, b = 0;
            for (unsigned char c : data) {
                a = (a + c) % 65521;
                b = (b + a) % 65521;
            }
            return (b << 16) | a;
        }

        // --- inflate, after Mark Adler's puff ---

        struct BitIn {
            const uint8_t *data;
            size_t size;
            size_t pos = 0;
            uint32_t buf = 0;
            int count = 0;
            bool error = false;

            int bits(int need) {
                uint32_t val = buf;
                while (count < need) {
                    if (pos >= size) {
                        error = true;
                        return 0;
                    }
                    val |= static_cast<uint32_t>(data[pos++]) << count;
                    count += 8;
                }
                buf = need < 32 ? val >> need : 0;
                count -= need;
                return static_cast<int>(val & ((1u << need) - 1));
            }
        };

        struct Huffman {
            short count[16];
            short symbol[288];
        };

        // false: an over-subscribed code
        bool construct(Huffman &h, const short *length, int n) {
            std::memset(h.count, 0, sizeof(h.count));
            for (int s = 0; s < n; s++) {
                h.count[length[s]]++;
            }
            if (h.count[0] == n) {
                return true;
            }
            int left = 1;
            for (int len = 1; len < 16; len++) {
                left <<= 1;
                left -= h.count[len];
                if (left < 0) {
                    return false;
                }
            }
            short offs[16];
            offs[1] = 0;
            for (int len = 1; len < 15; len++) {
                offs[len + 1] = static_cast<short>(offs[len] + h.count[len]);
            }
            for (int s = 0; s < n; s++) {
                if (length[s] != 0) {
                    h.symbol[offs[length[s]]++] = static_cast<short>(s);
                }
            }
            return true;
        }

        int decode(BitIn &in, const Huffman &h) {
            int code = 0, first = 0, index = 0;
            for (int len = 1; len < 16; len++) {
                code |= in.bits(1);
                if (in.error) {
                    return -1;
                }
                int count = h.count[len];
                if (code - count < first) {
                    return h.symbol[index + (code - first)];
                }
                index += count;
                first += count;
                first <<= 1;
                code <<= 1;
            }
            return -1;
        }

        bool codes(BitIn &in, std::string &out, const Huffman &lens, const Huffman &dists, size_t limit) {
            int sym;
            do {
                sym = decode(in, lens);
                if (sym < 0) {
                    return false;
                }
                if (sym < 256) {
                    out += static_cast<char>(sym);
                } else if (sym > 256) {
                    sym -= 257;
                    if (sym >= 29) {
                        return false;
                    }
                    int len = kLenBase[sym] + in.bits(kLenExtra[sym]);
                    int d = decode(in, dists);
                    if (d < 0 || d >= 30) {
                        return false;
                    }
                    size_t dist = static_cast<size_t>(kDistBase[d] + in.bits(kDistExtra[d]));
                    if (in.error || dist > out.size()) {
                        return false;
                    }
                    for (int i = 0; i < len; i++) {
                        out += out[out.size() - dist];
                    }
                }
                if (out.size() > limit) {
                    return false;
                }
            } while (sym != 256);
            return true;
        }

        bool inflateRaw(BitIn &in, std::string &out, size_t limit) {
            int last;
            do {
                last = in.bits(1);
                int type = in.bits(2);
                if (in.error) {
                    return false;
                }
                if (type == 0) {
                    in.buf = 0;
                    in.count = 0;
                    if (in.pos + 4 > in.size) {
                        return false;
                    }
                    unsigned len = in.data[in.pos] | (in.data[in.pos + 1] << 8);
                    unsigned nlen = in.data[in.pos + 2] | (in.data[in.pos + 3] << 8);
                    in.pos += 4;
                    if (len != (~nlen & 0xffff) || in.pos + len > in.size || out.size() + len > limit) {
                        return false;
                    }
                    out.append(reinterpret_cast<const char *>(in.data + in.pos), len);
                    in.pos += len;
                } else if (type == 1) {
                    static Huffman lens, dists;
                    static bool built = false;
                    if (!built) {
                        short length[288];
                        int s = 0;
                        for (; s < 144; s++) length[s] = 8;
                        for (; s < 256; s++) length[s] = 9;
                        for (; s < 280; s++) length[s] = 7;
                        for (; s < 288; s++) length[s] = 8;
                        construct(lens, length, 288);
                        for (s = 0; s < 30; s++) length[s] = 5;
                        construct(dists, length, 30);
                        built = true;
                    }
                    if (!codes(in, out, lens, dists, limit)) {
                        return false;
                    }
                } else if (type == 2) {
                    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                    int nlen = in.bits(5) + 257, ndist = in.bits(5) + 1, ncode = in.bits(4) + 4;
                    if (in.error || nlen > 286 || ndist > 30) {
                        return false;
                    }
                    short length[320] = {};
                    for (int i = 0; i < ncode; i++) {
                        length[order[i]] = static_cast<short>(in.bits(3));
                    }
                    Huffman lencode, distcode;
                    if (in.error || !construct(lencode, length, 19)) {
                        return false;
                    }
                    int index = 0;
                    while (index < nlen + ndist) {
                        int sym = decode(in, lencode);
                        if (sym < 0) {
                            return false;
                        }
                        if (sym < 16) {
                            length[index++] = static_cast<short>(sym);
                            continue;
                        }
                        short value = 0;
                        int repeat;
                        if (sym == 16) {
                            if (index == 0) {
                                return false;
                            }
                            value = length[index - 1];
                            repeat = 3 + in.bits(2);
                        } else if (sym == 17) {
                            repeat = 3 + in.bits(3);
                        } else {
                            repeat = 11 + in.bits(7);
                        }
                        if (in.error || index + repeat > nlen + ndist) {
                            return false;
                        }
                        while (repeat--) {
                            length[index++] = value;
                        }
                    }
                    if (length[256] == 0 || !construct(lencode, length, nlen) ||
                        !construct(distcode, length + nlen, ndist)) {
                        return false;
                    }
                    if (!codes(in, out, lencode, distcode, limit)) {
                        return false;
                    }
                } else {
                    return false;
                }
            } while (!last);
            return true;
        }

        // --- deflate: one block of the fixed codes, greedy LZ77 ---

        struct BitOut {
            std::string out;
            uint32_t buf = 0;
            int count = 0;

            void put(uint32_t value, int n) {
                buf |= value << count;
                count += n;
                while (count >= 8) {
                    out += static_cast<char>(buf & 0xff);
                    buf >>= 8;
                    count -= 8;
                }
            }

            // a Huffman code: its first bit the most significant
            void code(uint32_t value, int n) {
                uint32_t r = 0;
                for (int i = 0; i < n; i++) {
                    r = (r << 1) | ((value >> i) & 1);
                }
                put(r, n);
            }

            void flush() {
                if (count > 0) {
                    out += static_cast<char>(buf & 0xff);
                }
                buf = 0;
                count = 0;
            }
        };

        void literal(BitOut &o, int v) {
            if (v < 144) o.code(0x30 + v, 8);
            else if (v < 256) o.code(0x190 + v - 144, 9);
            else if (v < 280) o.code(v - 256, 7);
            else o.code(0xc0 + v - 280, 8);
        }

        void match(BitOut &o, int len, int dist) {
            int i = 28;
            while (kLenBase[i] > len) i--;
            literal(o, 257 + i);
            o.put(static_cast<uint32_t>(len - kLenBase[i]), kLenExtra[i]);
            int d = 29;
            while (kDistBase[d] > dist) d--;
            o.code(static_cast<uint32_t>(d), 5);
            o.put(static_cast<uint32_t>(dist - kDistBase[d]), kDistExtra[d]);
        }
    }

    bool zlibInflate(const std::string &in, std::string &out, size_t limit) {
        out.clear();
        const auto *d = reinterpret_cast<const uint8_t *>(in.data());
        if (in.size() < 6 || (d[0] & 0x0f) != 8 || ((d[0] << 8) | d[1]) % 31 != 0 || (d[1] & 0x20)) {
            return false;
        }
        BitIn bits{d + 2, in.size() - 2};
        if (!inflateRaw(bits, out, limit) || bits.pos + 4 > bits.size) {
            return false;
        }
        const uint8_t *a = bits.data + bits.pos;
        uint32_t sum = (static_cast<uint32_t>(a[0]) << 24) | (a[1] << 16) | (a[2] << 8) | a[3];
        return sum == adler32(out);
    }

    std::string zlibDeflate(const std::string &in) {
        BitOut o;
        o.out = "\x78\xda";
        o.put(1, 1);  // the last block
        o.put(1, 2);  // fixed codes
        const auto *d = reinterpret_cast<const uint8_t *>(in.data());
        const int n = static_cast<int>(in.size());
        std::vector<int> head(4096, -1), prev(static_cast<size_t>(n), -1);
        auto hashAt = [&](int i) { return ((d[i] << 8) ^ (d[i + 1] << 4) ^ d[i + 2]) & 4095; };
        auto insert = [&](int i) {
            if (i + 2 < n) {
                int h = hashAt(i);
                prev[static_cast<size_t>(i)] = head[static_cast<size_t>(h)];
                head[static_cast<size_t>(h)] = i;
            }
        };
        for (int i = 0; i < n;) {
            int best = 0, bestDist = 0;
            if (i + 2 < n) {
                int steps = 0;
                for (int j = head[static_cast<size_t>(hashAt(i))]; j >= 0 && i - j <= 32768 && steps < 128;
                     j = prev[static_cast<size_t>(j)], steps++) {
                    int len = 0;
                    while (len < 258 && i + len < n && d[j + len] == d[i + len]) {
                        len++;
                    }
                    if (len > best) {
                        best = len;
                        bestDist = i - j;
                    }
                }
            }
            if (best >= 3) {
                match(o, best, bestDist);
                for (int k = 0; k < best; k++) {
                    insert(i + k);
                }
                i += best;
            } else {
                literal(o, d[i]);
                insert(i);
                i++;
            }
        }
        literal(o, 256);
        o.flush();
        uint32_t sum = adler32(in);
        for (int s = 24; s >= 0; s -= 8) {
            o.out += static_cast<char>((sum >> s) & 0xff);
        }
        return o.out;
    }

    bool torrsUnpack(const std::string &link, Torrs &out) {
        std::string token = link.rfind("torrs://", 0) == 0 ? link.substr(8) : link;
        while (!token.empty() && (token.back() == '/' || std::isspace(static_cast<unsigned char>(token.back())))) {
            token.pop_back();
        }
        if (token.empty() || token.size() > 16384) {
            return false;
        }
        // base62 -> bytes (big-endian), as Go's big.Int: no leading zero bytes
        std::vector<uint8_t> num;
        for (char c : token) {
            const char *p = std::strchr(kBase62, c);
            if (!p || c == 0) {
                return false;
            }
            uint32_t carry = static_cast<uint32_t>(p - kBase62);
            for (auto it = num.rbegin(); it != num.rend(); ++it) {
                uint32_t v = *it * 62u + carry;
                *it = static_cast<uint8_t>(v & 0xff);
                carry = v >> 8;
            }
            while (carry) {
                num.insert(num.begin(), static_cast<uint8_t>(carry & 0xff));
                carry >>= 8;
            }
        }
        std::string data;
        if (!zlibInflate(std::string(num.begin(), num.end()), data, 1 << 16) || data.size() < 20) {
            return false;
        }
        static const char *hex = "0123456789abcdef";
        out = Torrs();
        for (int i = 0; i < 20; i++) {
            out.hash += hex[(static_cast<uint8_t>(data[i]) >> 4) & 15];
            out.hash += hex[static_cast<uint8_t>(data[i]) & 15];
        }
        size_t pos = 20;
        while (pos < data.size()) {
            const uint8_t tag = static_cast<uint8_t>(data[pos++]);
            if (tag == 4) {
                if (pos + 8 > data.size()) {
                    return false;
                }
                uint64_t v = 0;
                for (int i = 7; i >= 0; i--) {
                    v = (v << 8) | static_cast<uint8_t>(data[pos + static_cast<size_t>(i)]);
                }
                out.size = static_cast<int64_t>(v);
                pos += 8;
                continue;
            }
            if (pos + 2 > data.size()) {
                return false;
            }
            size_t len = static_cast<uint8_t>(data[pos]) | (static_cast<size_t>(static_cast<uint8_t>(data[pos + 1])) << 8);
            pos += 2;
            if (pos + len > data.size()) {
                return false;
            }
            std::string value = data.substr(pos, len);
            pos += len;
            if (tag == 0) out.title = value;
            else if (tag == 1) out.poster = value;
            else if (tag == 2) out.trackers.push_back(value);
            else if (tag == 3) out.category = value;
        }
        return true;
    }

    std::string torrsPack(const Torrs &t) {
        auto trim = [](std::string s) {
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            s.erase(s.find_last_not_of(" \t\r\n") + 1);
            return s;
        };
        std::string data;
        if (t.hash.size() != 40) {
            return "";
        }
        for (size_t i = 0; i < 40; i += 2) {
            data += static_cast<char>(std::stoi(t.hash.substr(i, 2), nullptr, 16));
        }
        auto field = [&](uint8_t tag, const std::string &raw) {
            std::string v = trim(raw);
            if (v.empty()) {
                return;
            }
            v = v.substr(0, 65535);
            data += static_cast<char>(tag);
            data += static_cast<char>(v.size() & 0xff);
            data += static_cast<char>((v.size() >> 8) & 0xff);
            data += v;
        };
        // Silo's order
        field(0, t.title);
        field(1, t.poster);
        field(3, t.category);
        if (t.size != 0) {
            data += static_cast<char>(4);
            uint64_t v = static_cast<uint64_t>(t.size);
            for (int i = 0; i < 8; i++) {
                data += static_cast<char>((v >> (8 * i)) & 0xff);
            }
        }
        for (const std::string &tr : t.trackers) {
            field(2, tr);
        }
        const std::string z = zlibDeflate(data);
        // bytes -> base62: leading zero bytes as '0' (as Silo's Encode62)
        std::vector<uint8_t> num(z.begin(), z.end());
        std::string zeros;
        size_t start = 0;
        while (start < num.size() && num[start] == 0) {
            zeros += '0';
            start++;
        }
        std::string digits;
        while (start < num.size()) {
            uint32_t rem = 0;
            for (size_t i = start; i < num.size(); i++) {
                uint32_t v = (rem << 8) | num[i];
                num[i] = static_cast<uint8_t>(v / 62);
                rem = v % 62;
            }
            digits += kBase62[rem];
            while (start < num.size() && num[start] == 0) {
                start++;
            }
        }
        std::reverse(digits.begin(), digits.end());
        return "torrs://" + zeros + digits;
    }

}
