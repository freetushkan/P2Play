#pragma once

// The log on the screen of a console that has no text console of its own (PS4, PS5 as a big
// app): the last lines drawn into a 32-bit framebuffer (A8R8G8B8), a title line on top. The
// platform owns the framebuffer and the flips (videoout.cpp); this part is the same everywhere.

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace p2p {

    class TextConsole {
    public:
        void setTitle(const std::string &title);

        // a log line (long ones wrapped when drawn)
        void add(const std::string &line);

        // something new since the last draw
        bool dirty() const { return m_dirty; }

        // pitch in pixels
        void draw(uint32_t *fb, int width, int height, int pitch);

    private:
        std::mutex m_mutex;
        std::string m_title;
        std::deque<std::string> m_lines;
        bool m_dirty = true;
    };

    // the platform's screen: started once there is a video out (false: none)
    bool screenStart(TextConsole *console);

    void screenStop();

}
