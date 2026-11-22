// scene_player: a player application that draws a scene through the RHI's bindless model
// (ADR-0009) in a render graph pass (ADR-0028), in a window, and prints its frame timings when it
// stops (Scenes.h).
//
//   scene_player                  10,000 instanced cubes, until the window is closed
//   scene_player --scene S        draws scene S: cubes (the default) or triangle
//   scene_player --frames N       stops after N frames
//   scene_player --backend B      renders with backend B: d3d12, vulkan or null (default: the
//                                 platform's, Direct3D 12 on Windows and Vulkan elsewhere)
//   scene_player --adapter A      renders on adapter A: hardware (the default, the fastest GPU)
//                                 or software (WARP on Direct3D 12, lavapipe on Vulkan)
//   scene_player --no-vsync       presents without waiting for vertical blank, so the frame time
//                                 is the time the frame takes
//
// Where the window has no native handle (SDL's headless "offscreen" driver) or no device can be
// created, it runs the event loop without rendering, about 60 times a second.

#include "ith/foundation/Version.h"
#include "ith/jobs/JobSystem.h"
#include "ith/platform/Platform.h"
#include "ith/render_graph/RenderGraph.h"
#include "ith/render_graph/ResourcePool.h"
#include "ith/rhi/CommandList.h"
#include "ith/rhi/Device.h"
#include "ith/rhi_select/CreateDevice.h"

#include "Scenes.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
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

using Clock = std::chrono::steady_clock;

enum class SceneKind : std::uint8_t {
    Triangle,
    Cubes
};

constexpr std::uint32_t kCubeCount = 10'000;
constexpr ith::rhi::Format kSwapchainFormat = ith::rhi::Format::B8G8R8A8Srgb;
constexpr std::array<float, 4> kClearColor{0.02f, 0.03f, 0.05f, 1.0f};

int fail(const ith::Error& error) {
    std::fprintf(stderr, "scene_player: %s (%s)\n", error.message.c_str(), error.code.c_str());
    return 1;
}

void report(const ith::rhi::Error& error) {
    std::fprintf(stderr, "scene_player: %s\n", error.toString().c_str());
}

// Milliseconds of a duration.
double milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

// Frame timings on the CPU. The RHI has no GPU timestamp queries yet, so GPU time is not
// measured; without vsync, the frame time is bounded by the GPU's time anyway.
struct Timings {
    std::uint64_t frames = 0;
    // From the start of one frame to the start of the next.
    Clock::duration frameTotal{};
    Clock::duration frameMin = Clock::duration::max();
    Clock::duration frameMax{};
    // Recording and submitting the frame's commands, after beginFrame() returned.
    Clock::duration recordTotal{};

    void add(Clock::duration frame, Clock::duration record) {
        ++frames;
        frameTotal += frame;
        frameMin = std::min(frameMin, frame);
        frameMax = std::max(frameMax, frame);
        recordTotal += record;
    }
};

// A device, the window's swapchain and the scene's GPU objects.
struct Renderer {
    ith::rhi::Device device;
    ith::rhi::SwapchainHandle swapchain;
    SceneKind kind = SceneKind::Cubes;
    scene_player::Triangle triangle;
    scene_player::Cubes cubes;
};

const char* backendName(ith::rhi::Backend backend) {
    switch (backend) {
        case ith::rhi::Backend::D3D12:
            return "Direct3D 12";
        case ith::rhi::Backend::Vulkan:
            return "Vulkan";
        case ith::rhi::Backend::Null:
            break;
    }
    return "the null backend";
}

// A renderer for the window, or nothing when the window can't be rendered to here.
std::optional<Renderer> createRenderer(const ith::Window& window, std::optional<ith::rhi::Backend> backend,
                                       ith::rhi::AdapterPreference adapter, SceneKind kind, bool vsync) {
    const ith::NativeWindowHandle handle = window.nativeHandle();
    if (std::holds_alternative<std::monostate>(handle)) {
        std::printf("scene_player: the video driver has no native window; running without rendering\n");
        return std::nullopt;
    }
    // Validation is on in Debug builds (Direct3D 12 needs the Windows "Graphics Tools" for it).
    ith::Expected<ith::rhi::Device, ith::rhi::Error> device =
        ith::rhi::createDevice({.adapter = adapter, .applicationName = "scene_player"}, backend);
    if (!device) {
        std::fprintf(stderr, "scene_player: %s; running without rendering\n", device.error().toString().c_str());
        return std::nullopt;
    }
    const ith::WindowSize pixels = window.pixelSize();
    ith::Expected<ith::rhi::SwapchainHandle, ith::rhi::Error> swapchain =
        device->createSwapchain({.window = handle,
                                 .width = static_cast<std::uint32_t>(pixels.width),
                                 .height = static_cast<std::uint32_t>(pixels.height),
                                 .format = kSwapchainFormat,
                                 .vsync = vsync,
                                 .debugName = "Main window"});
    if (!swapchain) {
        std::fprintf(stderr, "scene_player: %s; running without rendering\n", swapchain.error().toString().c_str());
        return std::nullopt;
    }
    Renderer renderer{.device = std::move(*device), .swapchain = *swapchain, .kind = kind};
    if (kind == SceneKind::Triangle) {
        ith::Expected<scene_player::Triangle, ith::rhi::Error> triangle =
            scene_player::createTriangle(renderer.device, kSwapchainFormat);
        if (!triangle) {
            report(triangle.error());
            renderer.device.destroy(renderer.swapchain);
            return std::nullopt;
        }
        renderer.triangle = *triangle;
    } else {
        ith::Expected<scene_player::Cubes, ith::rhi::Error> cubes =
            scene_player::createCubes(renderer.device, kSwapchainFormat, scene_player::cubeField(kCubeCount));
        if (!cubes) {
            report(cubes.error());
            renderer.device.destroy(renderer.swapchain);
            return std::nullopt;
        }
        renderer.cubes = *cubes;
    }
    const ith::rhi::Capabilities& capabilities = renderer.device.capabilities();
    std::printf("scene_player: rendering with %s on '%s'\n", backendName(capabilities.backend),
                capabilities.adapter.name.c_str());
    return renderer;
}

// Draws one frame of the scene through the render graph, which imports the swapchain image,
// takes the cubes' depth buffer from the pool (sized to the frame, so a resize gets a new one and
// the old one goes a few frames later) and derives every barrier. Returns false when the frame
// failed (device loss, or no memory for the depth buffer), after printing why. Draws nothing
// while the window is minimized; otherwise adds the time spent building, recording and
// submitting to `record`.
bool render(Renderer& renderer, ith::rg::RenderGraph& graph, ith::rg::ResourcePool& pool, double seconds,
            Clock::duration& record) {
    using namespace ith::rhi;
    using ith::Expected;
    namespace rg = ith::rg;
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
    const Clock::time_point start = Clock::now();
    const Frame& current = **frame;
    const bool cubes = renderer.kind == SceneKind::Cubes;
    const float aspect = static_cast<float>(current.width) / static_cast<float>(current.height);
    const ith::Mat4 viewProjection = scene_player::viewProjection(scene_player::fieldCamera(seconds), aspect);

    graph.reset();
    const rg::Texture backbuffer = graph.importBackbuffer(current);
    const rg::Texture depth = cubes ? graph.createTexture({.width = current.width,
                                                           .height = current.height,
                                                           .format = scene_player::kDepthFormat,
                                                           .debugName = "Depth"})
                                    : rg::Texture{};
    graph.addPass(
        cubes ? "Cubes" : "Triangle", rg::PassType::Raster,
        [&](rg::PassBuilder& pass) {
            pass.renderTarget(backbuffer, LoadOp::Clear, kClearColor);
            if (cubes) {
                pass.depth(depth);
            }
        },
        [&renderer, cubes, viewProjection](rg::PassContext& context) {
            if (cubes) {
                scene_player::draw(context.list(), context.device(), renderer.cubes, viewProjection);
            } else {
                scene_player::draw(context.list(), context.device(), renderer.triangle);
            }
        });
    graph.compile();
    if (Expected<void, Error> executed = graph.execute(pool); !executed) {
        // The backbuffer isn't in Present after a failed execute: unless the device is lost, a
        // graph that only imports it moves it there, so the frame ends with a valid present.
        if (!renderer.device.isLost()) {
            graph.reset();
            static_cast<void>(graph.importBackbuffer(current));
            graph.compile();
            static_cast<void>(graph.execute(pool));
        }
        static_cast<void>(renderer.device.endFrame());
        report(executed.error());
        return false;
    }
    record = Clock::now() - start;
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

void printTimings(const Renderer& renderer, const Timings& timings) {
    if (timings.frames == 0) {
        std::printf("scene_player: no frames were drawn\n");
        return;
    }
    const auto count = static_cast<double>(timings.frames);
    const double average = milliseconds(timings.frameTotal) / count;
    const char* scene = renderer.kind == SceneKind::Cubes ? "10,000 instanced cubes" : "triangle";
    std::printf("scene_player: %llu frames of the %s: frame %.3f ms average (min %.3f, max %.3f, "
                "%.0f fps), recording and submission %.3f ms on the CPU; GPU time not measured (no timestamp queries "
                "in the RHI yet)\n",
                static_cast<unsigned long long>(timings.frames), scene, average, milliseconds(timings.frameMin),
                milliseconds(timings.frameMax), 1000.0 / average, milliseconds(timings.recordTotal) / count);
}

// The command line.
struct Options {
    // 0 runs until the window is closed.
    std::uint64_t frameLimit = 0;
    // std::nullopt picks the platform's backend.
    std::optional<ith::rhi::Backend> backend;
    ith::rhi::AdapterPreference adapter = ith::rhi::AdapterPreference::HighPerformance;
    SceneKind scene = SceneKind::Cubes;
    bool vsync = true;
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
        const std::string_view value = i + 1 < arguments.size() ? std::string_view(arguments[i + 1]) : "";
        if (argument == "--frames" && !value.empty()) {
            ++i;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), options.frameLimit);
            if (error != std::errc() || end != value.data() + value.size() || options.frameLimit == 0) {
                std::fprintf(stderr, "scene_player: --frames needs a positive number\n");
                return std::nullopt;
            }
        } else if (argument == "--backend" && !value.empty()) {
            ++i;
            options.backend = parseBackend(value);
            if (!options.backend) {
                std::fprintf(stderr, "scene_player: --backend takes d3d12, vulkan or null\n");
                return std::nullopt;
            }
        } else if (argument == "--adapter" && (value == "hardware" || value == "software")) {
            ++i;
            options.adapter = value == "software" ? ith::rhi::AdapterPreference::Software
                                                  : ith::rhi::AdapterPreference::HighPerformance;
        } else if (argument == "--scene" && (value == "cubes" || value == "triangle")) {
            ++i;
            options.scene = value == "triangle" ? SceneKind::Triangle : SceneKind::Cubes;
        } else if (argument == "--no-vsync") {
            options.vsync = false;
        } else {
            std::fprintf(stderr,
                         "usage: scene_player [--scene cubes|triangle] [--frames N] [--backend d3d12|vulkan|null] "
                         "[--adapter hardware|software] [--no-vsync]\n");
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

    ith::Expected<ith::Platform> platform = ith::Platform::create();
    if (!platform) {
        return fail(platform.error());
    }
    int status = 0;
    {
        // Declared after the Platform, so it goes first: no wake reaches a Platform that shuts down.
        ith::JobSystem jobs;
        jobs.setMainThreadWake([](void* /*context*/) { ith::Platform::wake(); }, nullptr);
        ith::Expected<ith::Window> window = platform->createWindow({.title = "Ironthorn scene player"});
        if (!window) {
            return fail(window.error());
        }
        const ith::WindowSize pixels = window->pixelSize();
        std::printf("Ironthorn %.*s scene player: window %dx%d pixels\n",
                    static_cast<int>(ith::kEngineVersionString.size()), ith::kEngineVersionString.data(), pixels.width,
                    pixels.height);

        std::optional<Renderer> renderer =
            createRenderer(*window, options->backend, options->adapter, options->scene, options->vsync);
        // The frame's render graph, and the pool of its transient resources, which goes before the
        // device.
        ith::rg::RenderGraph graph({.name = "Frame"});
        std::optional<ith::rg::ResourcePool> pool;
        if (renderer) {
            pool.emplace(renderer->device);
        }
        const Clock::time_point start = Clock::now();
        Clock::time_point frameStart = start;
        Timings timings;
        std::uint64_t frames = 0;
        bool running = true;
        while (running && (options->frameLimit == 0 || frames < options->frameLimit)) {
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
            if (!renderer) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }
            const std::chrono::duration<double> elapsed = Clock::now() - start;
            Clock::duration record{};
            if (!render(*renderer, graph, *pool, elapsed.count(), record)) {
                status = 1;
                break;
            }
            // The first frame pays for startup (pipeline and swapchain warm-up), so it isn't timed.
            const Clock::time_point now = Clock::now();
            if (frames > 1 && record != Clock::duration{}) {
                timings.add(now - frameStart, record);
            }
            frameStart = now;
        }
        if (renderer) {
            // The swapchain goes before the window (Device.h); a lost device only needs destroying.
            static_cast<void>(renderer->device.waitIdle());
            printTimings(*renderer, timings);
            graph.reset();
            pool.reset();
            scene_player::destroy(renderer->device, renderer->triangle);
            scene_player::destroy(renderer->device, renderer->cubes);
            renderer->device.destroy(renderer->swapchain);
        }
        std::printf("scene player: %llu frames\n", static_cast<unsigned long long>(frames));
    }
    return status;
}
