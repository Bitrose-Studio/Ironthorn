// window_player: a player application that opens a window and runs the platform event loop
// until the window is closed or the application is asked to quit (ADR-0012).
//
//   window_player              runs until the window is closed
//   window_player --frames N   stops after N frames, e.g. headless in CI with the environment
//                              variable SDL_VIDEO_DRIVER=offscreen
//
// It draws nothing yet: there is no renderer. Each frame pumps the events and sleeps for about
// 16 ms. On Wayland the window therefore stays invisible: a Wayland window appears only once
// something is presented to it.

#include "ith/foundation/Version.h"
#include "ith/platform/Platform.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <system_error>
#include <thread>
#include <variant>

namespace {

const char* nativeHandleKind(const ith::NativeWindowHandle& handle) {
    if (std::holds_alternative<ith::Win32WindowHandle>(handle)) {
        return "win32";
    }
    if (std::holds_alternative<ith::WaylandWindowHandle>(handle)) {
        return "wayland";
    }
    if (std::holds_alternative<ith::X11WindowHandle>(handle)) {
        return "x11";
    }
    return "none";
}

int fail(const ith::Error& error) {
    std::fprintf(stderr, "window_player: %s (%s)\n", error.message.c_str(), error.code.c_str());
    return 1;
}

} // namespace

int main(int argc, char** argv) { // NOLINT(misc-const-correctness): the signature of main is fixed
    // 0 runs until the window is closed.
    std::uint64_t frameLimit = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--frames" && i + 1 < argc) {
            const std::string_view value(argv[++i]);
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), frameLimit);
            if (error != std::errc() || end != value.data() + value.size() || frameLimit == 0) {
                std::fprintf(stderr, "window_player: --frames needs a positive number\n");
                return 2;
            }
        } else {
            std::fprintf(stderr, "usage: window_player [--frames N]\n");
            return 2;
        }
    }

    ith::Expected<ith::Platform> platform = ith::Platform::create();
    if (!platform) {
        return fail(platform.error());
    }
    {
        ith::Expected<ith::Window> window = platform->createWindow({.title = "Ironthorn window player"});
        if (!window) {
            return fail(window.error());
        }
        const ith::WindowSize size = window->size();
        const ith::WindowSize pixels = window->pixelSize();
        const std::string_view driver = platform->videoDriver();
        std::printf("Ironthorn %.*s window player: driver %.*s, window %dx%d, %dx%d pixels, scale %.2f, native %s\n",
                    static_cast<int>(ith::kEngineVersionString.size()), ith::kEngineVersionString.data(),
                    static_cast<int>(driver.size()), driver.data(), size.width, size.height, pixels.width,
                    pixels.height, static_cast<double>(window->displayScale()),
                    nativeHandleKind(window->nativeHandle()));

        std::uint64_t frames = 0;
        bool running = true;
        while (running && (frameLimit == 0 || frames < frameLimit)) {
            for (const ith::Event& event : platform->pumpEvents()) {
                if (event.type == ith::EventType::QuitRequested ||
                    (event.type == ith::EventType::WindowCloseRequested && event.window == window->id())) {
                    running = false;
                }
            }
            ++frames;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        std::printf("window player: %llu frames\n", static_cast<unsigned long long>(frames));
    }
    return 0;
}
