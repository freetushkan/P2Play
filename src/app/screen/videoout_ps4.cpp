// The PS4's screen for the log (console.hpp): two framebuffers in direct memory, flipped by
// VideoOut on vsync, as OpenOrbis' samples do; redrawn when a line came, a frame a second
// otherwise (the screen stays alive).

#include <atomic>
#include <chrono>
#include <thread>

#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include "console.hpp"

namespace p2p {

    namespace {
        constexpr int kWidth = 1920, kHeight = 1080;
        std::atomic<bool> g_run{false};
        std::thread g_thread;

        void loop(TextConsole *console) {
            const int video = sceVideoOutOpen(0xFF, 0, 0, nullptr);
            if (video < 0) {
                return;
            }
            const size_t frame = (static_cast<size_t>(kWidth) * kHeight * 4 + 0x1FFFFF) & ~static_cast<size_t>(0x1FFFFF);
            off_t phys = 0;
            void *mem = nullptr;
            if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), frame * 2, 0x200000, 3, &phys) != 0 ||
                sceKernelMapDirectMemory(&mem, frame * 2, 0x33, 0, phys, 0x200000) != 0) {
                sceVideoOutClose(video);
                return;
            }
            void *buffers[2] = {mem, static_cast<char *>(mem) + frame};
            OrbisVideoOutBufferAttribute attr{};
            // A8R8G8B8 (sRGB), linear, 16:9
            sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1, 0, kWidth, kHeight, kWidth);
            OrbisKernelEqueue queue = 0;
            sceKernelCreateEqueue(&queue, "p2play flip");
            sceVideoOutAddFlipEvent(queue, video, nullptr);
            if (sceVideoOutRegisterBuffers(video, 0, buffers, 2, &attr) != 0) {
                sceVideoOutClose(video);
                return;
            }
            sceVideoOutSetFlipRate(video, 0);
            int current = 0;
            auto last = std::chrono::steady_clock::time_point();
            while (g_run) {
                const auto now = std::chrono::steady_clock::now();
                if (console->dirty() || now - last > std::chrono::seconds(1)) {
                    last = now;
                    console->draw(static_cast<uint32_t *>(buffers[current]), kWidth, kHeight, kWidth);
                    sceVideoOutSubmitFlip(video, current, 1, 0);
                    OrbisKernelEvent event;
                    int count = 0;
                    sceKernelWaitEqueue(queue, &event, 1, &count, nullptr);
                    current ^= 1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            sceVideoOutClose(video);
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
