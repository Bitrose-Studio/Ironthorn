#pragma once

// The scenes of scene_player, drawn with the bindless model of the RHI (ADR-0009): shaders read
// their vertices and instances from buffers through bindless indices passed as push constants,
// and pipelines have no vertex input layout. The GPU test (tests/scene_player) compiles this file
// too, renders the same scenes offscreen and checks the pixels.
//
//   triangle   three vertices with interpolated colors, in one draw
//   cubes      a field of cubes in one indexed, instanced draw, with a depth buffer and a
//              directional light
//
// The shaders are compiled from Triangle.slang and Cubes.slang at build time and embedded in the
// program (ith_embed_shaders, ADR-0023); a game loads cooked shaders instead (ADR-0011 rule 9). Vertex,
// index and instance data stay in Upload memory, which the GPU reads directly: simple, and small
// enough here not to matter. The sample and the test draw the scenes in a render graph pass
// (ADR-0028), which derives every barrier; nothing here records one.
//
// Creating and destroying are called on the thread that owns the device's frames (the render
// thread); draw() records into a list on any thread, as a render graph pass's execute callable.

#include "ith/foundation/Expected.h"
#include "ith/math/Types.h"
#include "ith/rhi/CommandList.h"
#include "ith/rhi/Device.h"
#include "ith/rhi/Error.h"
#include "ith/rhi/Format.h"
#include "ith/rhi/Handle.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace scene_player {

// The depth format of the cubes' pipeline and depth buffer. Depth is reversed (ADR-0006): clear
// it to 0, nearer is greater.
inline constexpr ith::rhi::Format kDepthFormat = ith::rhi::Format::D32Float;

// A camera of the engine's right-handed space (ADR-0006: +Y up, -Z forward).
struct Camera {
    ith::Vec3 eye{0.0f, 0.0f, 0.0f};
    ith::Vec3 target{0.0f, 0.0f, -1.0f};
    // Vertical field of view in radians, and the near plane's distance; positive.
    float verticalFov = 0.785398f;
    float nearPlane = 0.1f;
};

// World space to clip space for a camera and an aspect ratio (width / height, positive):
// ith::perspectiveReversedInfinite (depth 1 at the near plane, 0 at infinity) times glm::lookAt,
// in the engine's clip space, which the Vulkan backend shares by flipping its viewport.
// Precondition: the eye differs from the target, and they are not on one vertical line.
[[nodiscard]] ith::Mat4 viewProjection(const Camera& camera, float aspect);

// One cube, as Cubes.slang reads it: 32 bytes.
struct CubeInstance {
    // The cube's center in world space.
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    // The cube's edge length.
    float scale = 1.0f;
    // Linear RGB; alpha is ignored.
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
};

static_assert(sizeof(CubeInstance) == 32, "Cubes.slang reads instances of 32 bytes");

// `count` cubes on a square grid around the origin, resting on the plane y = 0, with sizes from
// 0.6 to 1 and colors that vary per cube. Instance i is in row i / side (the rows run from
// +Z, the front, to -Z) and column i % side, where side = ceil(sqrt(count)); the spacing is
// 1.5. The same count always gives the same cubes.
[[nodiscard]] std::vector<CubeInstance> cubeField(std::uint32_t count);

// A camera that looks down at a field of 10,000 cubes from its front, raised by 45 and set back
// by 95, and circles it once every 60 seconds.
[[nodiscard]] Camera fieldCamera(double seconds);

// The triangle's GPU objects.
struct Triangle {
    ith::rhi::PipelineHandle pipeline;
    ith::rhi::BufferHandle vertices;
};

// Creates the triangle's pipeline, for a color target of `colorFormat` and no depth, and its
// vertices: (-0.6, -0.5) red, (0.6, -0.5) green and (0, 0.6) blue in clip space, counter-clockwise.
// Fails with the device's error; nothing stays created then.
ith::Expected<Triangle, ith::rhi::Error> createTriangle(ith::rhi::Device& device, ith::rhi::Format colorFormat);

// Records the triangle's draw. Precondition: the list is rendering to one color target of the
// pipeline's format, without depth.
void draw(ith::rhi::CommandList& list, const ith::rhi::Device& device, const Triangle& triangle);

// Destroys the triangle's objects (deferred; null handles are skipped).
void destroy(ith::rhi::Device& device, const Triangle& triangle);

// The cubes' GPU objects.
struct Cubes {
    ith::rhi::PipelineHandle pipeline;
    ith::rhi::BufferHandle vertices;
    ith::rhi::BufferHandle indices;
    ith::rhi::BufferHandle instances;
    std::uint32_t instanceCount = 0;
};

// Creates the cubes' pipeline, for a color target of `colorFormat` and a kDepthFormat depth
// buffer (test and write, reversed: nearer wins, and back faces culled), a cube of edge 1 as 24
// vertices and 36 indices, and a buffer holding `instances`. Fails with the device's error;
// nothing stays created then. Precondition: `instances` is not empty.
ith::Expected<Cubes, ith::rhi::Error> createCubes(ith::rhi::Device& device, ith::rhi::Format colorFormat,
                                                  std::span<const CubeInstance> instances);

// Records every cube in one indexed draw of instanceCount instances, in the order they were
// given. Precondition: the list is rendering to one color target of the pipeline's format and a
// kDepthFormat depth buffer.
void draw(ith::rhi::CommandList& list, const ith::rhi::Device& device, const Cubes& cubes,
          const ith::Mat4& viewProjection);

// Destroys the cubes' objects (deferred; null handles are skipped).
void destroy(ith::rhi::Device& device, const Cubes& cubes);

} // namespace scene_player
