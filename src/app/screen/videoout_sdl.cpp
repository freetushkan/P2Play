// The screen for the log through SDL2's window framebuffer (the PS5 as a big application:
// its SDL2 draws a software surface through VideoOut), redrawn when a line came.

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <SDL2/SDL.h>

#include "console.hpp"

namespace p2p {

    namespace {
        std::atomic<bool> g_run{false};
        std::thread g_thread;

        void loop(TextConsole *console) {
            if (SDL_Init(SDL_INIT_VIDEO) != 0) {
                return;
            }
            SDL_Window *window = SDL_CreateWindow("P2Play", 0, 0, 1920, 1080, SDL_WINDOW_FULLSCREEN);
            SDL_Surface *surface = window ? SDL_GetWindowSurface(window) : nullptr;
            if (!surface) {
                if (window) {
                    SDL_DestroyWindow(window);
                }
                SDL_Quit();
                return;
            }
            const int w = surface->w, h = surface->h;
            std::vector<uint32_t> pixels(static_cast<size_t>(w) * h);
            SDL_Surface *src = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_ARGB8888);
            auto last = std::chrono::steady_clock::time_point();
            while (g_run) {
                SDL_Event event;
                while (SDL_PollEvent(&event)) {
                }
                const auto now = std::chrono::steady_clock::now();
                if (console->dirty() || now - last > std::chrono::seconds(1)) {
                    last = now;
                    console->draw(pixels.data(), w, h, w);
                    SDL_BlitSurface(src, nullptr, surface, nullptr);
                    SDL_UpdateWindowSurface(window);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            SDL_FreeSurface(src);
            SDL_DestroyWindow(window);
            SDL_Quit();
        }
    }

    bool screenStart(TextConsole *console) {
        if (g_run.exchange(true)) {
            return true;
        }
        g_thread = std::thread(loop, console);
        return true;
    }

    void screenStop() {
        if (g_run.exchange(false) && g_thread.joinable()) {
            g_thread.join();
        }
    }

}
