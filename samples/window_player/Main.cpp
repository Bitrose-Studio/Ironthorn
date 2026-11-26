// window_player: a player application that opens a window, runs the platform event loop
// (ADR-0012) and clears the window to a slowly changing color through the RHI (ADR-0009) until
// the window is closed or the application is asked to quit.
//
//   window_player              runs until the window is closed
//   window_player --frames N   stops after N frames, e.g. headless in CI with the environment
//                              variable SDL_VIDEO_DRIVER=offscreen
//   window_player --backend B  renders with backend B: d3d12, vulkan or null (default: the
//                              platform's, Direct3D 12 on Windows and Vulkan elsewhere)
//   window_player --adapter A  renders on adapter A: hardware (the default, the fastest GPU) or
//                              software (WARP on Direct3D 12, lavapipe on Vulkan)
//
// It renders where the window has a native handle and the backend can start; otherwise, as with
// the headless "offscreen" driver or without a GPU driver, it runs the event loop without
// rendering, about 60 times a second. On Wayland such a
// window stays invisible: a Wayland window appears only once something is presented to it.

#include "ith/foundation/Profile.h"
#include "ith/foundation/Version.h"
#include "ith/jobs/JobSystem.h"
#include "ith/platform/Platform.h"
#include "ith/rhi/CommandList.h"
#include "ith/rhi/Device.h"
#include "ith/rhi_select/CreateDevice.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
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

void report(const ith::rhi::Error& error) {
    std::fprintf(stderr, "window_player: %s\n", error.toString().c_str());
}

// A device and the window's swapchain.
struct Renderer {
    ith::rhi::Device device;
    ith::rhi::SwapchainHandle swapchain;
};

// A renderer for the window, or nothing when the window can't be rendered to here.
std::optional<Renderer> createRenderer(const ith::Window& window, std::optional<ith::rhi::Backend> backend,
                                       ith::rhi::AdapterPreference adapter) {
    const ith::NativeWindowHandle handle = window.nativeHandle();
    if (std::holds_alternative<std::monostate>(handle)) {
        std::printf("window_player: the video driver has no native window; running without rendering\n");
        return std::nullopt;
    }
    // Validation is on in Debug builds. On Direct3D 12 it needs the Windows "Graphics Tools"
    // (docs/development/building.md); without them the device fails, and this prints why. On
    // Vulkan it runs when the validation layer is installed.
    ith::Expected<ith::rhi::Device, ith::rhi::Error> device =
        ith::rhi::createDevice({.adapter = adapter, .applicationName = "window_player"}, backend);
    if (!device) {
        std::fprintf(stderr, "window_player: %s; running without rendering\n", device.error().toString().c_str());
        return std::nullopt;
    }
    const ith::WindowSize pixels = window.pixelSize();
    ith::Expected<ith::rhi::SwapchainHandle, ith::rhi::Error> swapchain =
        device->createSwapchain({.window = handle,
                                 .width = static_cast<std::uint32_t>(pixels.width),
                                 .height = static_cast<std::uint32_t>(pixels.height),
                                 .debugName = "Main window"});
    if (!swapchain) {
        std::fprintf(stderr, "window_player: %s; running without rendering\n", swapchain.error().toString().c_str());
        return std::nullopt;
    }
    const ith::rhi::Backend chosen = device->capabilities().backend;
    const char* name = "the null backend";
    if (chosen == ith::rhi::Backend::D3D12) {
        name = "Direct3D 12";
    } else if (chosen == ith::rhi::Backend::Vulkan) {
        name = "Vulkan";
    }
    std::printf("window_player: rendering with %s on '%s'\n", name, device->capabilities().adapter.name.c_str());
    return Renderer{.device = std::move(*device), .swapchain = *swapchain};
}

// Clears the swapchain image to a color that cycles over a few seconds. Returns false when the
// frame failed (device loss), after printing why. Draws nothing while the window is minimized.
bool render(Renderer& renderer, double seconds) {
    using namespace ith::rhi;
    using ith::Expected;
    Expected<std::optional<Frame>, Error> frame = renderer.device.beginFrame(renderer.swapchain);
    if (!frame) {
        report(frame.error());
        return false;
    }
    if (!frame->has_value()) {
        // Minimized: no vertical blank paces the loop.
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        return true;
    }
    const Frame& current = **frame;
    const auto wave = [&](double phase) { return static_cast<float>(0.5 + 0.5 * std::sin(seconds * 0.8 + phase)); };

    CommandList& list = renderer.device.openCommandList("Clear");
    list.barrier(TextureBarrier{.texture = current.backbuffer,
                                .stagesAfter = PipelineStage::ColorOutput,
                                .accessAfter = Access::RenderTargetWrite,
                                .layoutBefore = TextureLayout::Undefined,
                                .layoutAfter = TextureLayout::RenderTarget});
    const std::array colors{
        ColorAttachment{.texture = current.backbuffer, .clearColor = {wave(0.0), wave(2.1), wave(4.2), 1.0f}}};
    list.beginRendering({.colors = colors});
    list.endRendering();
    list.barrier(TextureBarrier{.texture = current.backbuffer,
                                .stagesBefore = PipelineStage::ColorOutput,
                                .accessBefore = Access::RenderTargetWrite,
                                .layoutBefore = TextureLayout::RenderTarget,
                                .layoutAfter = TextureLayout::Present});
    if (Expected<void, Error> submitted = renderer.device.submit(list); !submitted) {
        static_cast<void>(renderer.device.endFrame());
        report(submitted.error());
        return false;
    }
    if (Expected<void, Error> ended = renderer.device.endFrame(); !ended) {
        report(ended.error());
        return false;
    }
    return true;
}

// Keeps the swapchain's size in step with the window.
void resizeOnEvent(Renderer& renderer, const ith::Window& window, const ith::Event& event) {
    std::optional<ith::WindowSize> size;
    if (event.type == ith::EventType::WindowPixelSizeChanged) {
        size = ith::WindowSize{.width = event.width, .height = event.height};
    } else if (event.type == ith::EventType::WindowMinimized) {
        size = ith::WindowSize{}; // zero: draw nothing
    } else if (event.type == ith::EventType::WindowRestored) {
        size = window.pixelSize();
    }
    if (size) {
        renderer.device.resizeSwapchain(renderer.swapchain, static_cast<std::uint32_t>(size->width),
                                        static_cast<std::uint32_t>(size->height));
    }
}

// The command line.
struct Options {
    // 0 runs until the window is closed.
    std::uint64_t frameLimit = 0;
    // std::nullopt picks the platform's backend.
    std::optional<ith::rhi::Backend> backend;
    ith::rhi::AdapterPreference adapter = ith::rhi::AdapterPreference::HighPerformance;
};

std::optional<ith::rhi::Backend> parseBackend(std::string_view value) {
    if (value == "d3d12") {
        return ith::rhi::Backend::D3D12;
    }
    if (value == "vulkan") {
        return ith::rhi::Backend::Vulkan;
    }
    if (value == "null") {
        return ith::rhi::Backend::Null;
    }
    return std::nullopt;
}

// The options, or std::nullopt after printing what is wrong with them.
std::optional<Options> parseOptions(std::span<char* const> arguments) {
    Options options;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const std::string_view argument(arguments[i]);
        const bool hasValue = i + 1 < arguments.size();
        if (argument == "--frames" && hasValue) {
            const std::string_view value(arguments[++i]);
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), options.frameLimit);
            if (error != std::errc() || end != value.data() + value.size() || options.frameLimit == 0) {
                std::fprintf(stderr, "window_player: --frames needs a positive number\n");
                return std::nullopt;
            }
        } else if (argument == "--backend" && hasValue) {
            options.backend = parseBackend(arguments[++i]);
            if (!options.backend) {
                std::fprintf(stderr, "window_player: --backend takes d3d12, vulkan or null\n");
                return std::nullopt;
            }
        } else if (argument == "--adapter" && hasValue &&
                   (std::string_view(arguments[i + 1]) == "hardware" ||
                    std::string_view(arguments[i + 1]) == "software")) {
            options.adapter = std::string_view(arguments[++i]) == "software"
                                  ? ith::rhi::AdapterPreference::Software
                                  : ith::rhi::AdapterPreference::HighPerformance;
        } else {
            std::fprintf(
                stderr,
                "usage: window_player [--frames N] [--backend d3d12|vulkan|null] [--adapter hardware|software]\n");
            return std::nullopt;
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv) { // NOLINT(misc-const-correctness): the signature of main is fixed
    const std::optional<Options> options = parseOptions(std::span<char* const>(argv, static_cast<std::size_t>(argc)));
    if (!options) {
        return 2;
    }
    const auto [frameLimit, backend, adapter] = *options;

    ith::Expected<ith::Platform> platform = ith::Platform::create();
    if (!platform) {
        return fail(platform.error());
    }
    int status = 0;
    {
        // Declared after the Platform, so it goes first: no wake reaches a Platform that shuts down.
        ith::JobSystem jobs;
        jobs.setMainThreadWake([](void* /*context*/) { ith::Platform::wake(); }, nullptr);
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

        std::optional<Renderer> renderer = createRenderer(*window, backend, adapter);
        const auto start = std::chrono::steady_clock::now();
        std::uint64_t frames = 0;
        bool running = true;
        while (running && (frameLimit == 0 || frames < frameLimit)) {
            for (const ith::Event& event : platform->pumpEvents()) {
                if (event.type == ith::EventType::QuitRequested ||
                    (event.type == ith::EventType::WindowCloseRequested && event.window == window->id())) {
                    running = false;
                }
                if (renderer && event.window == window->id()) {
                    resizeOnEvent(*renderer, *window, event);
                }
            }
            // The frame order of ADR-0035 rule 22: events, then main-thread jobs, then update and render.
            jobs.runMainThreadJobs();
            ++frames;
            if (renderer) {
                // The swapchain waits for vertical blank, which paces the loop.
                const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
                if (!render(*renderer, elapsed.count())) {
                    status = 1;
                    break;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            ITH_PROFILE_FRAME();
        }
        if (renderer) {
            // The swapchain goes before the window (Device.h); a lost device only needs destroying.
            static_cast<void>(renderer->device.waitIdle());
            renderer->device.destroy(renderer->swapchain);
        }
        std::printf("window player: %llu frames\n", static_cast<unsigned long long>(frames));
    }
    return status;
}
