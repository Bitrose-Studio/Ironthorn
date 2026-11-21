// scene_player's scenes (samples/scene_player/Scenes.h) rendered offscreen on every GPU backend this
// machine has, read back and checked against what the geometry predicts: no reference images.
//
// - Direct3D 12 (Windows): WARP, or the fastest hardware GPU with ITH_RHI_TEST_ADAPTER=hardware;
//   with the debug layer and GPU-based validation outside Shipping (ITH_RHI_TEST_VALIDATION=off
//   turns them off, for machines without the "Graphics Tools").
// - Vulkan: the fastest hardware GPU, or a software one such as lavapipe
//   (ITH_RHI_TEST_ADAPTER=hardware or =software picks one), with the validation layer and
//   synchronization validation where it is installed. Without a Vulkan device its test reports
//   itself skipped, unless ITH_RHI_TEST_REQUIRE_VULKAN=1.
//
// Validation errors are asserted by the backends, so a scene that trips validation fails here.
// The suite exists only with the tools (IRONTHORN_BUILD_TOOLS), whose slangc compiles the scenes'
// shaders.

#include "ith/rhi/CommandList.h"
#include "ith/rhi/Device.h"
#include "ith/rhi_select/CreateDevice.h"

#include "Scenes.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <ostream> // doctest prints std::string_view operands through it
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace ith::rhi;
using ith::Expected;

namespace {

// An environment variable of this suite; empty when unset.
std::string environment(const char* name) {
#ifdef _WIN32
    // getenv_s: the C runtime deprecates getenv on Windows.
    std::array<char, 32> value{};
    std::size_t length = 0;
    if (getenv_s(&length, value.data(), value.size(), name) != 0 || length == 0) {
        return {};
    }
    return value.data();
#else
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe): no other thread runs
    return value != nullptr ? value : "";
#endif
}

bool testValidation() {
    return environment("ITH_RHI_TEST_VALIDATION") != "off";
}

// A device of `backend` for the test, or std::nullopt when this machine has none (reported, and a
// failure where the backend is required) or creation failed (recorded).
std::optional<Device> makeDevice(Backend backend) {
    const std::string adapter = environment("ITH_RHI_TEST_ADAPTER");
    const bool validation = testValidation();
    if (backend == Backend::D3D12) {
        if (defaultBackend() != Backend::D3D12) {
            MESSAGE("skipped: Direct3D 12 exists only on Windows");
            return std::nullopt;
        }
        Expected<Device, Error> device = createDevice(
            {.adapter = adapter == "hardware" ? AdapterPreference::HighPerformance : AdapterPreference::Software,
             .validation = validation,
             .gpuBasedValidation = validation,
             .applicationName = "scene_player tests"},
            Backend::D3D12);
        if (!device) {
            FAIL(device.error().toString());
            return std::nullopt;
        }
        return std::move(device).value();
    }
    std::vector<AdapterPreference> candidates;
    if (adapter != "software") {
        candidates.push_back(AdapterPreference::HighPerformance);
    }
    if (adapter != "hardware") {
        candidates.push_back(AdapterPreference::Software);
    }
    std::string reasons;
    for (const AdapterPreference candidate : candidates) {
        Expected<Device, Error> device = createDevice(
            {.adapter = candidate, .validation = validation, .applicationName = "scene_player tests"}, Backend::Vulkan);
        if (device) {
            return std::move(device).value();
        }
        reasons += " " + device.error().toString();
    }
    if (environment("ITH_RHI_TEST_REQUIRE_VULKAN") == "1") {
        FAIL("no Vulkan device, and ITH_RHI_TEST_REQUIRE_VULKAN=1 requires one:" << reasons);
    } else {
        MESSAGE("skipped: no Vulkan device:" << reasons);
    }
    return std::nullopt;
}

constexpr Format kColorFormat = Format::R8G8B8A8Unorm;
constexpr std::array<float, 4> kClearColor{0.1f, 0.2f, 0.3f, 1.0f};

// The light of Cubes.slang: brightness = 0.25 + 0.75 * max(0, normal . toLight).
constexpr std::array<float, 3> kToLight{0.3f, 0.8f, 0.5f};

// An RGBA8 image read back from a render target.
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rowPitch = 0;
    std::vector<std::byte> bytes;

    // The texel at (x, y); a failed check and an impossible texel outside the image.
    [[nodiscard]] std::array<int, 4> texel(std::uint32_t x, std::uint32_t y) const {
        CHECK_MESSAGE((x < width && y < height), "texel (" << x << ", " << y << ") is outside the image");
        if (x >= width || y >= height) {
            return {-1000, -1000, -1000, -1000};
        }
        const std::size_t offset = std::size_t{y} * rowPitch + std::size_t{x} * 4;
        return {std::to_integer<int>(bytes[offset]), std::to_integer<int>(bytes[offset + 1]),
                std::to_integer<int>(bytes[offset + 2]), std::to_integer<int>(bytes[offset + 3])};
    }
};

// A color in [0, 1] as the 8-bit texel an R8G8B8A8Unorm target stores.
std::array<int, 4> toTexel(const std::array<float, 4>& color) {
    std::array<int, 4> texel{};
    for (std::size_t channel = 0; channel < 4; ++channel) {
        texel[channel] = static_cast<int>(std::lround(std::clamp(color[channel], 0.0f, 1.0f) * 255.0f));
    }
    return texel;
}

bool similar(const std::array<int, 4>& actual, const std::array<int, 4>& expected, int tolerance) {
    for (std::size_t channel = 0; channel < 4; ++channel) {
        if (std::abs(actual[channel] - expected[channel]) > tolerance) {
            return false;
        }
    }
    return true;
}

// Renders `frames` frames into the same width x height target, with the same kDepthFormat depth
// buffer cleared to 0 when `withDepth`, and reads the last frame back; std::nullopt after
// recording a failure. Each frame begins from the previous one's use of both textures, which the
// barriers out of Undefined name (Barrier.h), so validation checks reuse across frames.
template <typename Draw>
std::optional<Image> render(Device& device, std::uint32_t width, std::uint32_t height, bool withDepth, Draw&& draw,
                            std::uint32_t frames = 1) {
    Image image{.width = width, .height = height, .rowPitch = width * 4};
    REQUIRE(image.rowPitch % kTextureCopyRowAlignment == 0);
    Expected<TextureHandle, Error> target =
        device.createTexture({.width = width,
                              .height = height,
                              .format = kColorFormat,
                              .usage = TextureUsage::RenderTarget | TextureUsage::CopySrc,
                              .debugName = "Scene target"});
    Expected<TextureHandle, Error> depth = device.createTexture({.width = width,
                                                                 .height = height,
                                                                 .format = scene_player::kDepthFormat,
                                                                 .usage = TextureUsage::DepthStencil,
                                                                 .debugName = "Scene depth"});
    Expected<BufferHandle, Error> readback = device.createBuffer({.size = std::uint64_t{image.rowPitch} * height,
                                                                  .usage = BufferUsage::CopyDst,
                                                                  .memory = MemoryType::Readback,
                                                                  .debugName = "Scene readback"});
    // Destroys what was created, on every path out.
    const auto release = [&] {
        if (target) {
            device.destroy(*target);
        }
        if (depth) {
            device.destroy(*depth);
        }
        if (readback) {
            device.destroy(*readback);
        }
    };
    if (!target || !depth || !readback) {
        FAIL("creating the target, depth buffer or readback buffer failed");
        release();
        return std::nullopt;
    }

    for (std::uint32_t frameIndex = 0; frameIndex < frames; ++frameIndex) {
        Expected<std::optional<Frame>, Error> frame = device.beginFrame();
        if (!frame || !frame->has_value()) {
            FAIL("beginFrame() failed");
            release();
            return std::nullopt;
        }
        const bool last = frameIndex + 1 == frames;
        CommandList& list = device.openCommandList("Scene");
        // The target was last read by the previous frame's copy, the depth buffer written.
        const std::array barriers{TextureBarrier{.texture = *target,
                                                 .stagesBefore = PipelineStage::Copy,
                                                 .stagesAfter = PipelineStage::ColorOutput,
                                                 .accessAfter = Access::RenderTargetWrite,
                                                 .layoutBefore = TextureLayout::Undefined,
                                                 .layoutAfter = TextureLayout::RenderTarget},
                                  scene_player::depthBarrier(*depth)};
        list.barrier(Barriers{.textures = std::span(barriers).first(withDepth ? 2 : 1)});
        const std::array colors{ColorAttachment{.texture = *target, .clearColor = kClearColor}};
        list.beginRendering({.colors = colors, .depth = {.texture = withDepth ? *depth : TextureHandle{}}});
        draw(list);
        list.endRendering();
        list.barrier(TextureBarrier{.texture = *target,
                                    .stagesBefore = PipelineStage::ColorOutput,
                                    .accessBefore = Access::RenderTargetWrite,
                                    .stagesAfter = PipelineStage::Copy,
                                    .accessAfter = Access::CopyRead,
                                    .layoutBefore = TextureLayout::RenderTarget,
                                    .layoutAfter = TextureLayout::CopySrc});
        if (last) {
            list.copyTextureToBuffer({.buffer = *readback, .rowPitch = image.rowPitch, .texture = *target});
        }
        const Expected<void, Error> submitted = device.submit(list);
        const Expected<void, Error> ended = device.endFrame();
        if (!submitted || !ended) {
            FAIL("rendering failed: " << (!submitted ? submitted.error().toString() : ended.error().toString()));
            release();
            return std::nullopt;
        }
    }
    if (const Expected<void, Error> idle = device.waitIdle(); !idle) {
        FAIL("rendering failed: " << idle.error().toString());
        release();
        return std::nullopt;
    }
    const std::span<const std::byte> memory = device.mappedMemory(*readback);
    image.bytes.assign(memory.begin(), memory.end());
    release();
    return image;
}

std::optional<Image> renderTriangle(Device& device) {
    Expected<scene_player::Triangle, Error> triangle = scene_player::createTriangle(device, kColorFormat);
    if (!triangle) {
        FAIL(triangle.error().toString());
        return std::nullopt;
    }
    std::optional<Image> image =
        render(device, 64, 64, false, [&](CommandList& list) { scene_player::draw(list, device, *triangle); });
    scene_player::destroy(device, *triangle);
    return image;
}

std::optional<Image> renderCubes(Device& device, std::span<const scene_player::CubeInstance> instances,
                                 const scene_player::Matrix& viewProjection, std::uint32_t width, std::uint32_t height,
                                 std::uint32_t frames = 1) {
    Expected<scene_player::Cubes, Error> cubes = scene_player::createCubes(device, kColorFormat, instances);
    if (!cubes) {
        FAIL(cubes.error().toString());
        return std::nullopt;
    }
    std::optional<Image> image = render(
        device, width, height, true,
        [&](CommandList& list) { scene_player::draw(list, device, *cubes, viewProjection); }, frames);
    scene_player::destroy(device, *cubes);
    return image;
}

// Where a world-space point lands: its pixel, and its clip-space w (the distance along the view).
struct Projected {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
};

Projected project(const scene_player::Matrix& m, const std::array<float, 3>& point, const Image& image) {
    std::array<double, 4> clip{};
    for (std::size_t row = 0; row < 4; ++row) {
        clip[row] = double{m[row]} * point[0] + double{m[4 + row]} * point[1] + double{m[8 + row]} * point[2] +
                    double{m[12 + row]};
    }
    return {.x = (clip[0] / clip[3] + 1.0) * 0.5 * image.width,
            .y = (1.0 - clip[1] / clip[3]) * 0.5 * image.height,
            .w = clip[3]};
}

// The texel a projected point lands on. Precondition (required): the point is inside the image.
std::array<int, 4> texelAt(const Image& image, const Projected& point) {
    REQUIRE_MESSAGE((point.x >= 0.0 && point.y >= 0.0 && point.x < image.width && point.y < image.height),
                    "(" << point.x << ", " << point.y << ") is outside the image");
    if (!(point.x >= 0.0 && point.y >= 0.0 && point.x < image.width && point.y < image.height)) {
        return {-1000, -1000, -1000, -1000};
    }
    return image.texel(static_cast<std::uint32_t>(point.x), static_cast<std::uint32_t>(point.y));
}

// The texel of an instance's color lit as a face with `normal`.
std::array<int, 4> litTexel(const std::array<float, 4>& color, const std::array<float, 3>& normal) {
    const float length = std::sqrt(kToLight[0] * kToLight[0] + kToLight[1] * kToLight[1] + kToLight[2] * kToLight[2]);
    const float facing = (normal[0] * kToLight[0] + normal[1] * kToLight[1] + normal[2] * kToLight[2]) / length;
    const float shade = 0.25f + 0.75f * std::max(facing, 0.0f);
    return toTexel({color[0] * shade, color[1] * shade, color[2] * shade, 1.0f});
}

// Whether a texel shows one face of a cube of `color`.
bool showsCube(const std::array<int, 4>& texel, const std::array<float, 4>& color) {
    constexpr std::array<std::array<float, 3>, 6> kNormals{
        {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
    return std::any_of(kNormals.begin(), kNormals.end(),
                       [&](const std::array<float, 3>& normal) { return similar(texel, litTexel(color, normal), 2); });
}

// The three scenes on one device.
struct Images {
    std::optional<Image> triangle;
    std::optional<Image> occlusion;
    std::optional<Image> field;
};

// Two cubes on the view axis, the near one first: a red cube of edge 1 at the origin, and a green
// cube of edge 4 behind it, seen from +Z.
const std::array<scene_player::CubeInstance, 2> kOcclusionCubes{{
    {.position = {0.0f, 0.0f, 0.0f}, .scale = 1.0f, .color = {1.0f, 0.0f, 0.0f, 1.0f}},
    {.position = {0.0f, 0.0f, -5.0f}, .scale = 4.0f, .color = {0.0f, 1.0f, 0.0f, 1.0f}},
}};
constexpr std::uint32_t kFieldCount = 10'000;
constexpr std::uint32_t kFieldWidth = 640;
constexpr std::uint32_t kFieldHeight = 360;

scene_player::Matrix occlusionView() {
    return scene_player::viewProjection({.eye = {0.0f, 0.0f, 5.0f}, .target = {0.0f, 0.0f, 0.0f}}, 1.0f);
}

scene_player::Matrix fieldView() {
    return scene_player::viewProjection(scene_player::fieldCamera(0.0),
                                        static_cast<float>(kFieldWidth) / static_cast<float>(kFieldHeight));
}

Images renderScenes(Device& device) {
    const std::vector<scene_player::CubeInstance> field = scene_player::cubeField(kFieldCount);
    return {.triangle = renderTriangle(device),
            .occlusion = renderCubes(device, kOcclusionCubes, occlusionView(), 64, 64),
            .field = renderCubes(device, field, fieldView(), kFieldWidth, kFieldHeight)};
}

// Clear color in the corners; the vertex colors' average at the centroid; red, green and blue at
// the corners' ends; and as many covered pixels as the triangle's area predicts.
void checkTriangle(const Image& image) {
    const std::array<int, 4> clear = toTexel(kClearColor);
    CHECK(similar(image.texel(0, 0), clear, 1));
    CHECK(similar(image.texel(63, 0), clear, 1));
    CHECK(similar(image.texel(0, 63), clear, 1));
    CHECK(similar(image.texel(63, 63), clear, 1));

    // Clip (x, y) to pixel: ((x + 1) * 32, (1 - y) * 32). The centroid is (0, -0.1333): pixel
    // (32, 36.3), where each vertex weighs about a third.
    const std::array<int, 4> centroid = image.texel(32, 36);
    CAPTURE(centroid);
    CHECK(similar(centroid, {85, 85, 85, 255}, 6));
    // Near each vertex, its own color dominates.
    CHECK(image.texel(14, 46)[0] > 180); // near (-0.6, -0.5), red
    CHECK(image.texel(48, 46)[1] > 180); // near (0.6, -0.5), green
    CHECK(image.texel(32, 15)[2] > 180); // near (0, 0.6), blue

    // The triangle covers 0.66 of the 4 units of clip space: 16.5% of 4096 pixels.
    int covered = 0;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            covered += similar(image.texel(x, y), clear, 1) ? 0 : 1;
        }
    }
    CAPTURE(covered);
    CHECK(std::abs(covered - 676) <= 20);
}

// The near red cube hides the far green one where both are, although it is drawn first: the
// depth test works, reversed. The green cube shows around it, and the clear color outside both.
void checkOcclusion(const Image& image) {
    const scene_player::Matrix view = occlusionView();
    const Projected center = project(view, {0.0f, 0.0f, 0.5f}, image);
    const std::array<int, 4> red = texelAt(image, center);
    CAPTURE(red);
    CHECK(similar(red, litTexel(kOcclusionCubes[0].color, {0, 0, 1}), 2));
    // A point on the green cube's front face (z = -3), above the red cube's outline.
    const Projected above = project(view, {0.0f, 1.2f, -3.0f}, image);
    const std::array<int, 4> green = texelAt(image, above);
    CAPTURE(green);
    CHECK(similar(green, litTexel(kOcclusionCubes[1].color, {0, 0, 1}), 2));
    CHECK(similar(image.texel(1, 1), toTexel(kClearColor), 1));
}

// 10,000 cubes in one instanced draw: the sky above the field is clear; and the cubes nearest to
// the camera, which nothing can hide, show their own color at their projected centers.
void checkField(const Image& image) {
    const std::array<int, 4> clear = toTexel(kClearColor);
    CHECK(similar(image.texel(0, 0), clear, 1));
    CHECK(similar(image.texel(image.width - 1, 0), clear, 1));

    const std::vector<scene_player::CubeInstance> field = scene_player::cubeField(kFieldCount);
    const scene_player::Matrix view = fieldView();
    std::vector<std::pair<Projected, std::size_t>> visible;
    for (std::size_t i = 0; i < field.size(); ++i) {
        const Projected center = project(view, field[i].position, image);
        if (center.x > image.width * 0.1 && center.x < image.width * 0.9 && center.y > image.height * 0.1 &&
            center.y < image.height * 0.9) {
            visible.emplace_back(center, i);
        }
    }
    REQUIRE(visible.size() > 1000);
    std::sort(visible.begin(), visible.end(), [](const auto& a, const auto& b) { return a.first.w < b.first.w; });
    for (std::size_t n = 0; n < std::min<std::size_t>(5, visible.size()); ++n) {
        const Projected center = visible[n].first;
        const std::size_t index = visible[n].second;
        const std::array<int, 4> texel = texelAt(image, center);
        CAPTURE(index);
        CAPTURE(texel);
        CHECK(showsCube(texel, field[index].color));
    }

    // Most of the picture shows cubes.
    std::size_t covered = 0;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            covered += similar(image.texel(x, y), clear, 1) ? 0 : 1;
        }
    }
    CAPTURE(covered);
    CHECK(covered > std::size_t{image.width} * image.height / 3);
}

void checkImages(const Images& images) {
    REQUIRE(images.triangle.has_value());
    REQUIRE(images.occlusion.has_value());
    REQUIRE(images.field.has_value());
    if (!images.triangle || !images.occlusion || !images.field) {
        return;
    }
    checkTriangle(*images.triangle);
    checkOcclusion(*images.occlusion);
    checkField(*images.field);
}

// The share of texels in which any channel differs by more than 2.
double differingShare(const Image& a, const Image& b) {
    std::size_t differing = 0;
    for (std::uint32_t y = 0; y < a.height; ++y) {
        for (std::uint32_t x = 0; x < a.width; ++x) {
            differing += similar(a.texel(x, y), b.texel(x, y), 2) ? 0 : 1;
        }
    }
    return static_cast<double>(differing) / (static_cast<double>(a.width) * a.height);
}

} // namespace

TEST_SUITE("scene_player") {
    TEST_CASE("Direct3D 12 renders the triangle, the occluding cubes and the field of 10,000 cubes") {
        std::optional<Device> device = makeDevice(Backend::D3D12);
        if (device) {
            checkImages(renderScenes(*device));
        }
    }

    TEST_CASE("Vulkan renders the triangle, the occluding cubes and the field of 10,000 cubes") {
        std::optional<Device> device = makeDevice(Backend::Vulkan);
        if (device) {
            checkImages(renderScenes(*device));
        }
    }

    // The sample's frame loop reuses one depth buffer, discarded by scene_player::depthBarrier()
    // after the earlier frames' writes: validation (GPU-based on Direct3D 12, synchronization on
    // Vulkan) checks that reuse, and the last frame must still be right.
    TEST_CASE("three frames into the same target and depth buffer pass validation on each backend") {
        for (const Backend backend : {Backend::D3D12, Backend::Vulkan}) {
            CAPTURE(static_cast<int>(backend));
            std::optional<Device> device = makeDevice(backend);
            if (!device) {
                continue;
            }
            const std::optional<Image> image = renderCubes(*device, kOcclusionCubes, occlusionView(), 64, 64, 3);
            REQUIRE(image.has_value());
            if (image) {
                checkOcclusion(*image);
            }
        }
    }

    TEST_CASE("Direct3D 12 and Vulkan render the same images") {
        std::optional<Device> d3d12 = makeDevice(Backend::D3D12);
        if (!d3d12) {
            return;
        }
        std::optional<Device> vulkan = makeDevice(Backend::Vulkan);
        if (!vulkan) {
            return;
        }
        const Images a = renderScenes(*d3d12);
        const Images b = renderScenes(*vulkan);
        REQUIRE((a.triangle && a.occlusion && a.field && b.triangle && b.occlusion && b.field));
        if (!(a.triangle && a.occlusion && a.field && b.triangle && b.occlusion && b.field)) {
            return;
        }
        // Edges may fall on either side of a pixel center between GPUs; nothing else may differ.
        const double triangle = differingShare(*a.triangle, *b.triangle);
        const double occlusion = differingShare(*a.occlusion, *b.occlusion);
        const double field = differingShare(*a.field, *b.field);
        MESSAGE("texels that differ: triangle " << triangle << ", occlusion " << occlusion << ", field " << field);
        CHECK(triangle < 0.01);
        CHECK(occlusion < 0.01);
        CHECK(field < 0.05);
    }
}
