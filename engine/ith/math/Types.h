#pragma once

// The engine's math types: glm types under engine names (ADR-0006).
//
//   ith::Vec3 position{1.0f, 0.0f, -2.0f};
//   ith::Quat rotation = glm::angleAxis(ith::kHalfPi, ith::kWorldUp); // ith/math/Math.h
//   ith::Mat4 world = glm::translate(ith::Mat4(1.0f), position);
//
// This header holds only the types and the axis constants. ith/math/Math.h adds glm's functions
// (normalize, cross, inverse, angleAxis, lookAt, ...) and the engine's projections. Engine code
// includes glm only through ith/math headers; the glm_includes test refuses a direct
// #include <glm/...> anywhere outside engine/ith/math.
//
// Conventions (ADR-0006), pinned by the math tests:
// - Right-handed, +X right, +Y up, forward is -Z. An identity rotation and a camera look down -Z.
// - SI units: 1 unit is 1 metre. Angles are radians.
// - Matrices are column-major with column vectors: v' = M * v, world = parent * local,
//   clip = projection * view * world. m[c] is column c, m[c][r] the element in row r.
// - Quaternions are unit Hamilton quaternions: q * v rotates v, a * b applies b first. The
//   constructor takes Quat(x, y, z, w), the order of its storage, glTF and shaders.
// - Default construction gives zero vectors, identity matrices and the identity quaternion.
//
// Layout and type traits: Vec3 is 12 bytes, Vec4 and Quat 16, Mat4 64, all with the alignment
// of their scalar, no padding, and the members x, y, z, w (no swizzles: glm is scalar in this
// configuration). The types are trivially copyable and standard-layout, so memcpy, GPU uploads
// and the ECS's trivially-copyable column path work. They are NOT trivially default-constructible:
// the zero/identity default is a user-provided constructor, so code must construct them (as
// std::vector, reflection's construct and the ECS do), never treat raw memory as one without
// constructing or copying into it.
//
// glm checks indices with assert() from <cassert>, which runs only in Debug (CMake defines NDEBUG
// in RelWithDebInfo and Release) and doesn't reach the engine's assert handler. These checks are
// left as they are: glm's GLM_ASSERT_LENGTH can't be redirected, and redefining assert() would
// change it for every header. Code that indexes a vector or matrix with a value computed at run
// time checks the index with ITH_ASSERT itself.

#include <glm/ext/matrix_double4x4.hpp>
#include <glm/ext/matrix_float3x3.hpp>
#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/quaternion_float.hpp>
#include <glm/ext/vector_double3.hpp>
#include <glm/ext/vector_float2.hpp>
#include <glm/ext/vector_float3.hpp>
#include <glm/ext/vector_float4.hpp>
#include <glm/ext/vector_int2.hpp>
#include <glm/ext/vector_int3.hpp>
#include <glm/ext/vector_int4.hpp>
#include <glm/ext/vector_uint2.hpp>
#include <glm/ext/vector_uint3.hpp>
#include <glm/ext/vector_uint4.hpp>

#include <numbers>

namespace ith {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using IVec2 = glm::ivec2; // std::int32_t components
using IVec3 = glm::ivec3;
using IVec4 = glm::ivec4;
using UVec2 = glm::uvec2; // std::uint32_t components
using UVec3 = glm::uvec3;
using UVec4 = glm::uvec4;
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;
using Quat = glm::quat;
// Double precision, for computations that need it (large-world positions are decided by a later
// ADR; world positions are Vec3 for now).
using DVec3 = glm::dvec3;
using DMat4 = glm::dmat4;

// The axes of world space (ADR-0006 rule 4).
inline constexpr Vec3 kWorldRight{1.0f, 0.0f, 0.0f};
inline constexpr Vec3 kWorldUp{0.0f, 1.0f, 0.0f};
inline constexpr Vec3 kWorldForward{0.0f, 0.0f, -1.0f};

inline constexpr float kPi = std::numbers::pi_v<float>;
inline constexpr float kTwoPi = 2.0f * kPi;
inline constexpr float kHalfPi = 0.5f * kPi;

} // namespace ith
