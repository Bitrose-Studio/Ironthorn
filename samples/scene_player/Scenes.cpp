#include "Scenes.h"

#include "ith/foundation/Assert.h"
#include "ith/math/Math.h"
#include "ith/rhi/Capabilities.h"
#include "ith/rhi/Pipeline.h"
#include "ith/rhi/Resources.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

namespace scene_player {
namespace {

// The shaders, compiled at build time by ith_embed_shaders: k<Entry>Spirv and, where the
// Direct3D 12 backend is built, k<Entry>Dxil, each an array of bytes with its size.
namespace embedded {
using std::size_t; // the generated headers name it unqualified
#include "cubePixel.spirv.h"
#include "cubeVertex.spirv.h"
#include "trianglePixel.spirv.h"
#include "triangleVertex.spirv.h"
#ifdef ITH_SCENE_PLAYER_HAS_DXIL
#include "cubePixel.dxil.h"
#include "cubeVertex.dxil.h"
#include "trianglePixel.dxil.h"
#include "triangleVertex.dxil.h"
#endif
} // namespace embedded

using namespace ith::rhi;

std::span<const std::byte> bytes(const unsigned char* data, std::size_t size) {
    return {reinterpret_cast<const std::byte*>(data), size};
}

// The bytecode of every entry point in one format.
struct Shaders {
    std::span<const std::byte> triangleVertex;
    std::span<const std::byte> trianglePixel;
    std::span<const std::byte> cubeVertex;
    std::span<const std::byte> cubePixel;
};

// The shaders in the device's format: DXIL for Direct3D 12, otherwise SPIR-V (Vulkan, and the null
// backend, which takes any bytes). DXIL is compiled only where the Direct3D 12 backend is built.
Shaders shaders([[maybe_unused]] const Device& device) {
#ifdef ITH_SCENE_PLAYER_HAS_DXIL
    if (device.capabilities().shaderFormat == ShaderFormat::Dxil) {
        return {.triangleVertex = bytes(embedded::kTriangleVertexDxil, embedded::kTriangleVertexDxil_sizeInBytes),
                .trianglePixel = bytes(embedded::kTrianglePixelDxil, embedded::kTrianglePixelDxil_sizeInBytes),
                .cubeVertex = bytes(embedded::kCubeVertexDxil, embedded::kCubeVertexDxil_sizeInBytes),
                .cubePixel = bytes(embedded::kCubePixelDxil, embedded::kCubePixelDxil_sizeInBytes)};
    }
#endif
    return {.triangleVertex = bytes(embedded::kTriangleVertexSpirv, embedded::kTriangleVertexSpirv_sizeInBytes),
            .trianglePixel = bytes(embedded::kTrianglePixelSpirv, embedded::kTrianglePixelSpirv_sizeInBytes),
            .cubeVertex = bytes(embedded::kCubeVertexSpirv, embedded::kCubeVertexSpirv_sizeInBytes),
            .cubePixel = bytes(embedded::kCubePixelSpirv, embedded::kCubePixelSpirv_sizeInBytes)};
}

// The push constants of each pipeline, laid out as the shaders declare them.
struct TriangleConstants {
    std::uint32_t vertices = 0;
};

struct CubeConstants {
    ith::Mat4 viewProjection;
    std::uint32_t vertices = 0;
    std::uint32_t instances = 0;
};

static_assert(sizeof(CubeConstants) == 72, "Cubes.slang's constants are 72 bytes");

struct TriangleVertex {
    std::array<float, 4> position;
    std::array<float, 4> color;
};

struct CubeVertex {
    ith::Vec3 position;
    ith::Vec3 normal;
};

static_assert(sizeof(CubeVertex) == 24, "Cubes.slang reads vertices of 24 bytes");

// A buffer filled with `data`, in Upload memory.
template <typename T>
ith::Expected<BufferHandle, Error> upload(Device& device, std::span<const T> data, BufferUsage usage,
                                          std::string_view debugName) {
    ith::Expected<BufferHandle, Error> buffer = device.createBuffer(
        {.size = data.size_bytes(), .usage = usage, .memory = MemoryType::Upload, .debugName = debugName});
    if (buffer) {
        std::memcpy(device.mappedMemory(*buffer).data(), data.data(), data.size_bytes());
    }
    return buffer;
}

// A cube of edge 1 around the origin: four vertices and two counter-clockwise triangles per face,
// seen from outside, so each face has its own normal.
void makeCube(std::vector<CubeVertex>& vertices, std::vector<std::uint16_t>& indices) {
    // Each face's normal and one axis across the face; the other axis is normal x axis, so that
    // axis x other = normal and the corners below run counter-clockwise.
    constexpr std::array<std::pair<ith::Vec3, ith::Vec3>, 6> kFaces{{
        {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {{-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},
        {{0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f}},
    }};
    constexpr std::array<std::array<float, 2>, 4> kCorners{
        {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}}};
    for (const auto& [normal, axis] : kFaces) {
        const ith::Vec3 other = glm::cross(normal, axis);
        const auto first = static_cast<std::uint16_t>(vertices.size());
        for (const auto& [u, v] : kCorners) {
            vertices.push_back({.position = 0.5f * (normal + u * axis + v * other), .normal = normal});
        }
        constexpr std::array<std::uint16_t, 6> kTriangles{0, 1, 2, 0, 2, 3};
        for (const std::uint16_t corner : kTriangles) {
            indices.push_back(static_cast<std::uint16_t>(first + corner));
        }
    }
}

// A well-mixed 32-bit hash of an index, for per-cube variation.
std::uint32_t hash(std::uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

// 8 bits of a hash as a float in [low, 1].
float unit(std::uint32_t bits, float low) {
    return low + (1.0f - low) * static_cast<float>(bits & 0xFFu) / 255.0f;
}

} // namespace

ith::Mat4 viewProjection(const Camera& camera, float aspect) {
    ITH_ASSERT_MSG(glm::length(glm::cross(glm::normalize(camera.target - camera.eye), ith::kWorldUp)) > 1e-6f,
                   "the camera looks straight up or down, or its eye is its target");
    return ith::perspectiveReversedInfinite(camera.verticalFov, aspect, camera.nearPlane) *
           glm::lookAt(camera.eye, camera.target, ith::kWorldUp);
}

std::vector<CubeInstance> cubeField(std::uint32_t count) {
    const auto side = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(count))));
    constexpr float kSpacing = 1.5f;
    const float half = 0.5f * kSpacing * static_cast<float>(side - 1);
    std::vector<CubeInstance> instances;
    instances.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t mixed = hash(i);
        const float scale = unit(mixed >> 24, 0.6f);
        const std::uint32_t row = i / side;
        const std::uint32_t column = i % side;
        instances.push_back({.position = {static_cast<float>(column) * kSpacing - half, 0.5f * scale,
                                          half - static_cast<float>(row) * kSpacing},
                             .scale = scale,
                             .color = {unit(mixed, 0.15f), unit(mixed >> 8, 0.15f), unit(mixed >> 16, 0.15f), 1.0f}});
    }
    return instances;
}

Camera fieldCamera(double seconds) {
    constexpr double kTurnsPerSecond = 1.0 / 60.0;
    constexpr double kTwoPi = 6.283185307179586;
    const double angle = seconds * kTurnsPerSecond * kTwoPi;
    return {.eye = {static_cast<float>(95.0 * std::sin(angle)), 45.0f, static_cast<float>(95.0 * std::cos(angle))},
            .target = {0.0f, 0.0f, 0.0f}};
}

ith::Expected<Triangle, Error> createTriangle(Device& device, Format colorFormat) {
    const Shaders code = shaders(device);
    const std::array shaders{
        ShaderDesc{.stage = ShaderStage::Vertex,
                   .code = code.triangleVertex,
                   .entryPoint = "triangleVertex",
                   .constantsSize = sizeof(TriangleConstants)},
        ShaderDesc{.stage = ShaderStage::Pixel, .code = code.trianglePixel, .entryPoint = "trianglePixel"}};
    const std::array targets{ColorTarget{.format = colorFormat}};
    Triangle triangle;
    ith::Expected<PipelineHandle, Error> pipeline =
        device.createGraphicsPipeline({.shaders = shaders, .colorTargets = targets, .debugName = "Triangle"});
    if (!pipeline) {
        return ith::makeUnexpected(std::move(pipeline).error());
    }
    triangle.pipeline = *pipeline;

    const std::array<TriangleVertex, 3> vertices{{
        {.position = {-0.6f, -0.5f, 0.0f, 1.0f}, .color = {1.0f, 0.0f, 0.0f, 1.0f}},
        {.position = {0.6f, -0.5f, 0.0f, 1.0f}, .color = {0.0f, 1.0f, 0.0f, 1.0f}},
        {.position = {0.0f, 0.6f, 0.0f, 1.0f}, .color = {0.0f, 0.0f, 1.0f, 1.0f}},
    }};
    ith::Expected<BufferHandle, Error> buffer =
        upload(device, std::span<const TriangleVertex>(vertices), BufferUsage::ShaderRead, "Triangle vertices");
    if (!buffer) {
        destroy(device, triangle);
        return ith::makeUnexpected(std::move(buffer).error());
    }
    triangle.vertices = *buffer;
    return triangle;
}

void draw(CommandList& list, const Device& device, const Triangle& triangle) {
    list.setPipeline(triangle.pipeline);
    list.pushConstants(TriangleConstants{.vertices = device.shaderReadIndex(triangle.vertices)});
    list.draw(3);
}

void destroy(Device& device, const Triangle& triangle) {
    device.destroy(triangle.pipeline);
    device.destroy(triangle.vertices);
}

ith::Expected<Cubes, Error> createCubes(Device& device, Format colorFormat, std::span<const CubeInstance> instances) {
    ITH_ASSERT(!instances.empty());
    const Shaders code = shaders(device);
    const std::array shaders{
        ShaderDesc{.stage = ShaderStage::Vertex,
                   .code = code.cubeVertex,
                   .entryPoint = "cubeVertex",
                   .constantsSize = sizeof(CubeConstants)},
        ShaderDesc{.stage = ShaderStage::Pixel, .code = code.cubePixel, .entryPoint = "cubePixel"}};
    const std::array targets{ColorTarget{.format = colorFormat}};
    Cubes cubes;
    cubes.instanceCount = static_cast<std::uint32_t>(instances.size());
    ith::Expected<PipelineHandle, Error> pipeline =
        device.createGraphicsPipeline({.shaders = shaders,
                                       .colorTargets = targets,
                                       .depthFormat = kDepthFormat,
                                       .depth = {.test = true, .write = true, .compare = CompareOp::Greater},
                                       .debugName = "Cubes"});
    if (!pipeline) {
        return ith::makeUnexpected(std::move(pipeline).error());
    }
    cubes.pipeline = *pipeline;

    std::vector<CubeVertex> cubeVertices;
    std::vector<std::uint16_t> cubeIndices;
    makeCube(cubeVertices, cubeIndices);
    // Each step stops at the first failure; the objects created until then are destroyed.
    std::optional<Error> failure;
    const auto keep = [&](ith::Expected<BufferHandle, Error> buffer, BufferHandle& handle) {
        if (failure) {
            return;
        }
        if (!buffer) {
            failure = std::move(buffer).error();
            return;
        }
        handle = *buffer;
    };
    keep(upload(device, std::span<const CubeVertex>(cubeVertices), BufferUsage::ShaderRead, "Cube vertices"),
         cubes.vertices);
    keep(upload(device, std::span<const std::uint16_t>(cubeIndices), BufferUsage::Index, "Cube indices"),
         cubes.indices);
    keep(upload(device, instances, BufferUsage::ShaderRead, "Cube instances"), cubes.instances);
    if (failure) {
        destroy(device, cubes);
        return ith::makeUnexpected(std::move(*failure));
    }
    return cubes;
}

void draw(CommandList& list, const Device& device, const Cubes& cubes, const ith::Mat4& viewProjection) {
    constexpr std::uint32_t kCubeIndices = 36;
    list.setPipeline(cubes.pipeline);
    list.setIndexBuffer(cubes.indices, 0, IndexFormat::Uint16);
    list.pushConstants(CubeConstants{.viewProjection = viewProjection,
                                     .vertices = device.shaderReadIndex(cubes.vertices),
                                     .instances = device.shaderReadIndex(cubes.instances)});
    list.drawIndexed(kCubeIndices, cubes.instanceCount);
}

void destroy(Device& device, const Cubes& cubes) {
    device.destroy(cubes.pipeline);
    device.destroy(cubes.vertices);
    device.destroy(cubes.indices);
    device.destroy(cubes.instances);
}

} // namespace scene_player
