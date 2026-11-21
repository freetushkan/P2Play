// PS4: whether the initialized data segment reads back as linked.
// The asio hash_map bucket table (a static inside an inline function, its own .data.* section)
// read as zeros on the first runs: SIGFPE in hash_map::insert.

#include <cstdint>
#include <cstdio>
#include <string>

extern "C" const std::size_t p2p_asio_hash_sizes[23]
    __asm__("_ZZN5boost4asio6detail8hash_mapIiNS1_16reactor_op_queueIiE11mapped_typeEE9hash_sizeEmE5sizes");

namespace {
    // 4 KB of known values in plain .data
    volatile std::uint32_t g_probe[1024] = {
#define P2P_P8(n) n, n + 1, n + 2, n + 3, n + 4, n + 5, n + 6, n + 7
#define P2P_P64(n) P2P_P8(n), P2P_P8(n + 8), P2P_P8(n + 16), P2P_P8(n + 24), P2P_P8(n + 32), P2P_P8(n + 40), P2P_P8(n + 48), P2P_P8(n + 56)
#define P2P_P256(n) P2P_P64(n), P2P_P64(n + 64), P2P_P64(n + 128), P2P_P64(n + 192)
            P2P_P256(0x1000), P2P_P256(0x1100), P2P_P256(0x1200), P2P_P256(0x1300)
    };
}

std::string p2pDataProbe() {
    char buf[512];
    int bad = 0, first = -1;
    for (int i = 0; i < 1024; i++) {
        if (g_probe[i] != static_cast<std::uint32_t>(0x1000 + i)) {
            if (first < 0) {
                first = i;
            }
            bad++;
        }
    }
    snprintf(buf, sizeof(buf),
             "data probe: g_probe@%p bad %d (first %d, [0]=0x%x [1023]=0x%x); asio sizes@%p = %zu %zu %zu %zu",
             (const void *) g_probe, bad, first, (unsigned) g_probe[0], (unsigned) g_probe[1023],
             (const void *) p2p_asio_hash_sizes, p2p_asio_hash_sizes[0], p2p_asio_hash_sizes[1],
             p2p_asio_hash_sizes[2], p2p_asio_hash_sizes[22]);
    return buf;
}
