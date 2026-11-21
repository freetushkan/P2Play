// DejaVu Sans Mono (Bitstream Vera license), 20 px, ASCII 32-126 as 12x24 8-bit coverage
// masks, made by tools/make-font.py
#pragma once
#include <cstdint>
namespace p2p {
    constexpr int kFontW = 12, kFontH = 24, kFontFirst = 32, kFontLast = 126;
    extern const uint8_t kFont[];
}
