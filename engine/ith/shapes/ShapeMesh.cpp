#include "ith/shapes/ShapeMesh.h"

#include "ith/foundation/Assert.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ith {

namespace {

constexpr float kHalf = 0.5f;
constexpr std::uint32_t kSegments = 16; // around Y, for the sphere and the cylinder
constexpr std::uint32_t kRings = 12;    // from pole to pole, for the sphere

// Indices are 16-bit, so every mesh must have at most 65535 vertices: the sphere has
// (kRings + 1) * (kSegments + 1), the cylinder at most 4 * (kSegments + 1) + 2.
static_assert((kRings + 1) * (kSegments + 1) <= 0xFFFF && 4 * (kSegments + 1) + 2 <= 0xFFFF,
              "shape meshes must fit 16-bit indices");

struct MeshData {
    std::vector<ShapeVertex> vertices;
    std::vector<std::uint16_t> indices;

    [[nodiscard]] std::uint16_t next() const noexcept { return static_cast<std::uint16_t>(vertices.size()); }

    void triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        indices.push_back(static_cast<std::uint16_t>(a));
        indices.push_back(static_cast<std::uint16_t>(b));
        indices.push_back(static_cast<std::uint16_t>(c));
    }

    // A flat square of side 1 centred at normal * offset, spanned by `u` and `v` with cross(u, v)
    // == normal, so that its corners in the order -u-v, +u-v, +u+v, -u+v are counter-clockwise seen
    // from the side the normal points to.
    void square(const Vec3& normal, const Vec3& u, const Vec3& v, float offset) {
        const std::uint32_t first = next();
        const Vec3 centre = normal * offset;
        vertices.push_back({centre - kHalf * u - kHalf * v, normal});
        vertices.push_back({centre + kHalf * u - kHalf * v, normal});
        vertices.push_back({centre + kHalf * u + kHalf * v, normal});
        vertices.push_back({centre - kHalf * u + kHalf * v, normal});
        triangle(first, first + 1, first + 2);
        triangle(first, first + 2, first + 3);
    }
};

// The direction around Y at segment `j`: (cos, 0, sin) of j / kSegments of a turn. With the
// segment index growing, a quad (j, bottom), (j, top), (j + 1, bottom) is counter-clockwise seen
// from outside.
Vec3 around(std::uint32_t j) {
    const float angle = kTwoPi * static_cast<float>(j % kSegments) / static_cast<float>(kSegments);
    return {std::cos(angle), 0.0f, std::sin(angle)};
}

MeshData makeCube() {
    const Vec3 x{1.0f, 0.0f, 0.0f};
    const Vec3 y{0.0f, 1.0f, 0.0f};
    const Vec3 z{0.0f, 0.0f, 1.0f};
    MeshData mesh;
    mesh.square(x, y, z, kHalf);
    mesh.square(-x, z, y, kHalf);
    mesh.square(y, z, x, kHalf);
    mesh.square(-y, x, z, kHalf);
    mesh.square(z, x, y, kHalf);
    mesh.square(-z, y, x, kHalf);
    return mesh;
}

MeshData makePlane() {
    MeshData mesh;
    mesh.square({0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, 0.0f);
    return mesh;
}

// Rings from the top pole (ring 0) to the bottom one (ring kRings), each with kSegments + 1
// vertices (the last repeats the first, so every quad has its own pair). The quads touching a
// pole lose their degenerate triangle.
MeshData makeSphere() {
    constexpr std::uint32_t kRowLength = kSegments + 1;
    MeshData mesh;
    for (std::uint32_t i = 0; i <= kRings; ++i) {
        const float polar = kPi * static_cast<float>(i) / static_cast<float>(kRings);
        float ringRadius = std::sin(polar);
        float height = std::cos(polar);
        if (i == 0 || i == kRings) { // exact poles: sin(pi) is not exactly 0 in float
            ringRadius = 0.0f;
            height = i == 0 ? 1.0f : -1.0f;
        }
        for (std::uint32_t j = 0; j <= kSegments; ++j) {
            const Vec3 normal = ringRadius * around(j) + Vec3(0.0f, height, 0.0f);
            mesh.vertices.push_back({kHalf * normal, normal});
        }
    }
    for (std::uint32_t i = 0; i < kRings; ++i) {
        for (std::uint32_t j = 0; j < kSegments; ++j) {
            const std::uint32_t a = i * kRowLength + j; // this ring
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + kRowLength; // the ring below
            const std::uint32_t d = c + 1;
            if (i != 0) {
                mesh.triangle(a, b, c);
            }
            if (i != kRings - 1) {
                mesh.triangle(b, d, c);
            }
        }
    }
    return mesh;
}

MeshData makeCylinder() {
    MeshData mesh;
    // The side: a bottom and a top vertex per segment, with the first pair repeated at the end.
    for (std::uint32_t j = 0; j <= kSegments; ++j) {
        const Vec3 normal = around(j);
        mesh.vertices.push_back({kHalf * normal - Vec3(0.0f, kHalf, 0.0f), normal});
        mesh.vertices.push_back({kHalf * normal + Vec3(0.0f, kHalf, 0.0f), normal});
    }
    for (std::uint32_t j = 0; j < kSegments; ++j) {
        const std::uint32_t bottom = 2 * j;
        const std::uint32_t top = bottom + 1;
        mesh.triangle(bottom, top, bottom + 2);
        mesh.triangle(top, top + 2, bottom + 2);
    }
    // The caps: a centre and a ring each.
    for (const float side : {1.0f, -1.0f}) {
        const Vec3 normal{0.0f, side, 0.0f};
        const std::uint32_t centre = mesh.next();
        mesh.vertices.push_back({kHalf * normal, normal});
        for (std::uint32_t j = 0; j < kSegments; ++j) {
            mesh.vertices.push_back({kHalf * around(j) + kHalf * normal, normal});
        }
        for (std::uint32_t j = 0; j < kSegments; ++j) {
            const std::uint32_t current = centre + 1 + j;
            const std::uint32_t following = centre + 1 + (j + 1) % kSegments;
            if (side > 0.0f) {
                mesh.triangle(centre, following, current);
            } else {
                mesh.triangle(centre, current, following);
            }
        }
    }
    return mesh;
}

static_assert(sizeof(ShapeVertex) == 24, "ShapeVertex is two packed Vec3s");

ShapeMesh view(const MeshData& mesh) noexcept {
    return {.vertices = mesh.vertices, .indices = mesh.indices};
}

} // namespace

// Each case builds its mesh on first use. No default: a new enumerator must get a case here
// (-Wswitch); a value outside the enumerators falls through to the assert.
ShapeMesh shapeMesh(Shape shape) noexcept {
    switch (shape) {
        case Shape::Cube: {
            static const MeshData kMesh = makeCube();
            return view(kMesh);
        }
        case Shape::Sphere: {
            static const MeshData kMesh = makeSphere();
            return view(kMesh);
        }
        case Shape::Plane: {
            static const MeshData kMesh = makePlane();
            return view(kMesh);
        }
        case Shape::Cylinder: {
            static const MeshData kMesh = makeCylinder();
            return view(kMesh);
        }
    }
    ITH_ASSERT_MSG(false, "shapes: not a Shape enumerator");
    return {};
}

Aabb shapeBounds(Shape shape) noexcept {
    const Aabb unit{.min = {-kHalf, -kHalf, -kHalf}, .max = {kHalf, kHalf, kHalf}};
    switch (shape) {
        case Shape::Cube:
        case Shape::Sphere:
        case Shape::Cylinder:
            return unit;
        case Shape::Plane:
            return {.min = {-kHalf, 0.0f, -kHalf}, .max = {kHalf, 0.0f, kHalf}};
    }
    ITH_ASSERT_MSG(false, "shapes: not a Shape enumerator");
    return {.min = unit.max, .max = unit.min};
}

} // namespace ith
