#include "console.hpp"

#include <algorithm>
#include <vector>

#include "font.hpp"

namespace p2p {

    namespace {
        constexpr uint32_t kBack = 0xFF141826, kText = 0xFFE6E2D8, kTitleBack = 0xFFF2711C, kTitleText = 0xFF242842;
        constexpr uint32_t kError = 0xFFFF8A65, kWeb = 0xFF9CC6FF;
        constexpr size_t kKeep = 400;

        uint32_t blend(uint32_t back, uint32_t fore, uint8_t a) {
            auto ch = [&](int s) {
                const uint32_t b = (back >> s) & 0xFF, f = (fore >> s) & 0xFF;
                return ((f * a + b * (255 - a)) / 255) << s;
            };
            return 0xFF000000 | ch(16) | ch(8) | ch(0);
        }

        void glyph(uint32_t *fb, int pitch, int x, int y, char c, uint32_t back, uint32_t fore) {
            if (c < kFontFirst || c > kFontLast) {
                c = '?';
            }
            const uint8_t *m = kFont + (c - kFontFirst) * kFontW * kFontH;
            for (int gy = 0; gy < kFontH; gy++) {
                uint32_t *row = fb + (y + gy) * pitch + x;
                for (int gx = 0; gx < kFontW; gx++) {
                    const uint8_t a = m[gy * kFontW + gx];
                    if (a) {
                        row[gx] = blend(back, fore, a);
                    }
                }
            }
        }

        void fill(uint32_t *fb, int pitch, int x, int y, int w, int h, uint32_t color) {
            for (int j = 0; j < h; j++) {
                std::fill(fb + (y + j) * pitch + x, fb + (y + j) * pitch + x + w, color);
            }
        }

        void text(uint32_t *fb, int pitch, int x, int y, const std::string &s, size_t cols, uint32_t back, uint32_t fore) {
            for (size_t i = 0; i < s.size() && i < cols; i++) {
                if (s[i] != ' ') {
                    glyph(fb, pitch, x + static_cast<int>(i) * kFontW, y, s[i], back, fore);
                }
            }
        }

        // UTF-8 to what the font has (one '?' a character outside ASCII)
        std::string ascii(const std::string &s) {
            std::string out;
            for (size_t i = 0; i < s.size(); i++) {
                const unsigned char c = static_cast<unsigned char>(s[i]);
                if (c < 0x80) {
                    out += c == '\t' ? ' ' : static_cast<char>(c);
                } else if ((c & 0xC0) != 0x80) {
                    out += '?';
                }
            }
            return out;
        }
    }

    void TextConsole::setTitle(const std::string &title) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_title = ascii(title);
        m_dirty = true;
    }

    void TextConsole::add(const std::string &line) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lines.push_back(ascii(line));
        while (m_lines.size() > kKeep) {
            m_lines.pop_front();
        }
        m_dirty = true;
    }

    void TextConsole::draw(uint32_t *fb, int width, int height, int pitch) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_dirty = false;
        // a safe area of 5 % (TVs crop the edges)
        const int mx = width / 20, my = height / 20;
        const size_t cols = static_cast<size_t>((width - 2 * mx) / kFontW);
        const int rows = (height - 2 * my) / kFontH - 2;
        fill(fb, pitch, 0, 0, width, height, kBack);
        fill(fb, pitch, mx - 8, my - 4, width - 2 * mx + 16, kFontH + 8, kTitleBack);
        text(fb, pitch, mx, my, m_title, cols, kTitleBack, kTitleText);
        // the newest lines that fit, wrapped
        std::vector<std::string> shown;
        for (auto it = m_lines.rbegin(); it != m_lines.rend() && static_cast<int>(shown.size()) < rows; ++it) {
            std::vector<std::string> parts;
            for (size_t p = 0; p < it->size() || parts.empty(); p += cols) {
                parts.push_back(it->substr(p, cols));
            }
            for (auto pt = parts.rbegin(); pt != parts.rend() && static_cast<int>(shown.size()) < rows; ++pt) {
                shown.push_back(*pt);
            }
        }
        int y = my + 2 * kFontH;
        for (auto it = shown.rbegin(); it != shown.rend(); ++it, y += kFontH) {
            const bool err = it->find("error") != std::string::npos || it->find("failed") != std::string::npos ||
                             it->find("cannot") != std::string::npos;
            text(fb, pitch, mx, y, *it, cols, kBack, err ? kError : it->find("[Web]") != std::string::npos ? kWeb : kText);
        }
    }

}
