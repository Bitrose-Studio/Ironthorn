# Math: types, conventions and helpers

The `math` module (layer `foundation`, kind `runtime`) gives the engine one set of math types and
one set of spatial conventions ([ADR-0006](../decisions/0006-math-and-coordinate-conventions.md)).
The types are [glm](https://github.com/g-truc/glm) 1.0.3 under engine names; the module adds the
projection glm lacks, `Transform`, bounds, frustums and ray tests.

## Use

```cpp
#include "ith/math/Math.h"      // types, glm's functions, perspectiveReversedInfinite
#include "ith/math/Geometry.h"  // Aabb, Sphere, Plane, Frustum, Ray and their tests
#include "ith/math/Transform.h" // ith::Transform

const ith::Mat4 view = glm::lookAt(eye, target, ith::kWorldUp);
const ith::Mat4 projection = ith::perspectiveReversedInfinite(glm::radians(60.0f), aspect, 0.1f);
const ith::Frustum frustum = ith::makeFrustum(projection * view);

const ith::Transform local{.translation = {0.0f, 1.0f, 0.0f},
                           .rotation = glm::angleAxis(ith::kHalfPi, ith::kWorldUp)};
const ith::Mat4 world = parentWorld * toMatrix(local);
if (ith::intersects(frustum, ith::transformBounds(meshBounds, world))) { /* draw */ }
```

- `ith/math/Types.h` holds only the types and the axis constants. Include it in headers that
  only declare math values, such as component structs; it is cheaper than `Math.h`.
- Use glm's functions (`glm::normalize`, `glm::cross`, `glm::inverse`, `glm::angleAxis`,
  `glm::mat4_cast`, `glm::value_ptr`, ...) through `ith/math/Math.h`. The experimental `gtx`
  headers are not available.
- **Never include `<glm/...>` directly.** Only files in `engine/ith/math` may; the `glm_includes`
  test fails for any other file in `engine`, `samples`, `tests`, `tools` or `editor`. This keeps
  the configuration below in every translation unit and the `ith` names as the engine's
  vocabulary.

| Engine name | glm type | |
|---|---|---|
| `Vec2`, `Vec3`, `Vec4` | `vec2`, `vec3`, `vec4` | float |
| `IVec2`, `IVec3`, `IVec4` | `ivec2`, ... | `std::int32_t` |
| `UVec2`, `UVec3`, `UVec4` | `uvec2`, ... | `std::uint32_t` |
| `Mat3`, `Mat4` | `mat3`, `mat4` | float, column-major |
| `Quat` | `quat` | float, unit Hamilton quaternion |
| `DVec3`, `DMat4` | `dvec3`, `dmat4` | double |

## Conventions

The `math` tests pin every one of these.

- **Space:** right-handed, +X right, **+Y up**, forward is **−Z** (`kWorldRight`, `kWorldUp`,
  `kWorldForward`). An identity rotation and a camera look down −Z. glTF needs no conversion.
- **Units:** 1 unit is 1 metre; kilograms and seconds. **Angles are radians** everywhere in code;
  degrees exist only in the UI and in human-edited files (`glm::radians` where they are read).
- **Matrices:** column-major, column vectors: `v' = M * v`. `m[c]` is column c, `m[c][r]` the
  element in row r, and the translation is `m[3]`. Products apply the right operand first:
  `world = parent * local`, `clip = projection * view * world`. Shaders receive matrices
  unchanged and use `mul(M, v)`.
- **Quaternions:** `q * v` rotates v; `a * b` applies b first. The constructor is
  **`Quat(x, y, z, w)`**, the order of its storage, glTF and shaders. Positive angles turn
  counter-clockwise looking down the axis: 90° about +Y turns +X into −Z.
- **Defaults:** default construction gives zero vectors, identity matrices and the identity
  quaternion.
- **Clip space:** x and y in [−1, 1] with +Y up, depth in [0, 1], on every backend (the Vulkan
  backend flips its viewport). Front faces wind counter-clockwise.
- **Depth:** scene cameras use `perspectiveReversedInfinite`: depth is `near / distance`, 1 at
  the near plane and approaching 0 far away, with no far plane. Pair it with a 32-bit float depth
  buffer cleared to 0 and a greater-or-equal test. Shadow maps and other conventional views use
  `glm::perspective` and `glm::ortho`, which map near to 0 and far to 1 here.
- **Colour** math is linear RGB.

## Configuration

`ith::glm` is an INTERFACE target that carries glm's include directory (as a system include)
and its configuration as interface compile definitions, so no file can include glm without
them. Only `math` links it, publicly.

| Define | Effect |
|---|---|
| `GLM_FORCE_DEPTH_ZERO_TO_ONE` | projections produce depth in [0, 1] |
| `GLM_FORCE_CTOR_INIT` | zero and identity defaults |
| `GLM_FORCE_XYZW_ONLY` | plain `x, y, z, w` members, no swizzles; glm is scalar (no SIMD) |
| `GLM_FORCE_QUAT_DATA_XYZW` | `Quat(x, y, z, w)` |

Right-handed is glm's default; no handedness define is set. Never add a SIMD or alignment define
(`GLM_FORCE_INTRINSICS`, `GLM_FORCE_SSE*`, `GLM_FORCE_AVX*`,
`GLM_FORCE_DEFAULT_ALIGNED_GENTYPES`): each changes layout or `constexpr`-ness and needs an ADR.
Hot loops that need SIMD use it explicitly in their own modules, or run on the GPU.

### Asserts

glm checks indices with `assert()` from `<cassert>`. These checks are left as they are:

- they run only in Debug, because CMake defines `NDEBUG` in RelWithDebInfo and Release;
- they don't reach the engine's assert handler (`ITH_ASSERT`), so a failure aborts like any C
  assert;
- they can't be redirected cleanly: glm's `GLM_ASSERT_LENGTH` calls `assert` directly, and
  redefining `assert` would change it for every header in the translation unit.

Code that indexes a vector or matrix with a value computed at run time checks the index with
`ITH_ASSERT` itself. The module's own functions assert their preconditions with `ITH_ASSERT`.

## Layout, reflection, the ECS and the GPU

- `Vec3` is 12 bytes, `Vec4` and `Quat` 16, `Mat4` 64, aligned like their scalar, without
  padding. The types are **trivially copyable** and standard-layout: `memcpy`, GPU uploads
  (`CommandList::pushConstants`, mapped buffers) and the ECS's `memcpy` path for trivially
  copyable columns all work.
- They are **not trivially default-constructible**: the zero and identity defaults come from a
  user-provided constructor. Always construct them (as `std::vector`, reflection's `construct`
  and the ECS do); never treat raw or zeroed memory as a matrix without constructing or copying
  into it. A zeroed `Mat4` is not the identity.
- **Reflection** describes the math types by hand as structs (`ith/reflection/TypeOf.h`), and
  every new `TypeRegistry` holds them, so they can be fields of reflected types and components:
  vectors and `Quat` have the fields `x, y, z, w`, `Mat3` and `Mat4` the columns `c0` to `c3`,
  and `Transform` `translation, rotation, scale`. The text archive writes a `Vec3` as
  `{"x": 1, "y": 2, "z": 3}`. `DMat4` is not reflected.

## Geometry

- `Aabb` (`min`, `max`; empty when min > max on an axis), `Sphere`, `Plane`
  (`dot(normal, p) + distance = 0`) and `Ray` (`origin + t * direction`).
- `makeFrustum(clipFromSpace)` extracts six inward planes from any clip-space matrix, reversed
  or conventional. With `perspectiveReversedInfinite` the depth-0 plane is at infinity and
  rejects nothing. `intersects(frustum, box or sphere)` is conservative: false only when the
  shape is entirely behind one plane.
- `transformBounds(box, matrix)` encloses a box moved by an affine matrix.
- `intersect(ray, box, plane or sphere)` returns the smallest `t >= 0`, or `nullopt`; a ray that
  starts inside a box or sphere hits at 0.

`Transform` is a plain value: the ECS component that places entities is the ECS's to define.
Hierarchies compose matrices, since two transforms with non-uniform scale don't compose into a
transform.

## Tests

- `math` (`tests/math`): the conventions above, the projections against known points, the
  geometry tests, and the math types in reflection, both archives and a `World`.
- `glm_includes` (lint) scans the repository for direct glm includes;
  `glm_includes_refuses_direct_include` checks that it reports a fixture that has them.
- The `scene_player` GPU test draws through `perspectiveReversedInfinite` and `glm::lookAt` on
  every available backend and checks the pixels against the same matrix.
