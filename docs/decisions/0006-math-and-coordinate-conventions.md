# ADR-0006: Math library and coordinate conventions

- **Status:** Accepted
- **Date:** 2026-11-20
- **Related:** [ADR-0001](0001-cpp-language-and-safety-rules.md) (no exceptions, no RTTI),
  [ADR-0004](0004-modules-and-layering.md) (layering, rule 5), the planned RHI, shader pipeline
  and large-world ADRs, [architecture](../architecture.md)

## Context

Every subsystem, shader and importer depends on one set of math types and spatial conventions.
Inconsistent conventions cause the most expensive class of bugs: mirrored meshes, inverted
rotations, depth-precision artifacts and per-backend projection fixes. They must be fixed before
the first RHI and asset code in P1.

Constraints:

- Math belongs to the `foundation` layer (ADR-0004). The existing `foundation` module may depend
  only on the C++ standard library.
- No exceptions anywhere and no RTTI in runtime modules (ADR-0001). Consoles must stay possible,
  and Clang is the reference compiler. Warnings are errors.
- No third-party types in public module headers unless an ADR allows it (ADR-0004 rule 5).
- Vulkan 1.3 and Direct3D 12 backends behind one engine RHI. Shaders are Slang.
- glTF is the primary interchange format. Jolt is the likely physics engine.
- Open worlds: the P7 test world is 2 × 2 km. Large coordinates need camera-relative rendering.

Lessons from earlier engines by the same maintainer:

- An engine using glm 1.0 behind type aliases ran for a long time with the conventions below
  (right-handed, +Y up, metres, column vectors, Direct3D-style clip space, reversed depth). Its
  renderer tests asserted pixels on both Vulkan and Direct3D 12. The glm configuration macros had
  to be identical in every translation unit, so they were set as public compile definitions of
  the math target, not in a header.
- A second engine reached the same conventions independently, and its pixel tests confirmed that
  a negative Vulkan viewport height with counter-clockwise front faces and reversed infinite
  depth work as stated in Decision 6.

## Options considered

### Math library

- **A. glm 1.0.3**, exposed through engine type aliases. MIT (or the "Happy Bunny" modified MIT)
  ([license](https://github.com/g-truc/glm/blob/master/copying.txt)). Header-only, maintained
  since 2005, latest release 1.0.3 ([releases](https://github.com/g-truc/glm/releases)); 1.1
  will require C++17. Verified in the 1.0.3 source: no `throw`, `typeid` or `dynamic_cast`;
  bounds checks use `assert`. Column-major matrices and column vectors, right-handed by default,
  depth [0, 1] with `GLM_FORCE_DEPTH_ZERO_TO_ONE`, double types (`dvec3`, `dmat4`)
  ([manual](https://github.com/g-truc/glm/blob/master/manual.md)). SIMD is opt-in
  (`GLM_FORCE_INTRINSICS`, SSE2–AVX512 and NEON) and turns off `constexpr`. Downsides: global
  configuration macros, large headers, and its API becomes engine API.
- **B. glm wrapped in engine-owned types.** Keeps ADR-0004 rule 5 intact, but needs hundreds of
  forwarding functions and conversions at every boundary. Same result as A at much higher cost.
- **C. DirectXMath.** MIT, Microsoft, native on Xbox, SSE/AVX/NEON
  ([repository](https://github.com/microsoft/DirectXMath)). Row vectors and separate storage
  (`XMFLOAT3`) and compute (`XMVECTOR`) types make gameplay code verbose. Non-Windows builds need
  a `sal.h` shim, and Linux is not listed as a supported platform. No double types. The current
  version number could not be verified.
- **D. Realtime Math (RTM).** MIT, C++11 headers, float and double, SSE/AVX/NEON, tested on
  Windows, Linux, macOS, iOS and Android ([repository](https://github.com/nfrechette/rtm)).
  Row vectors and a C-style function API built for animation compression. A good kernel library,
  a poor general-purpose gameplay API. Consoles are not mentioned.
- **E. Our own library.** Vectors, matrices, quaternions and their tests are a few thousand
  lines. Full control of layout, `constexpr` and asserts, and no rule-5 exception. But it is work
  before any engine work, with no result glm doesn't already give, and fresh code carries fresh
  bugs.

### Conventions

- **Handedness and up axis.** Right-handed +Y up matches glTF
  ([§3.4](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#coordinate-system-and-units):
  right-handed, +Y up, front of an asset faces +Z, metres), glm's defaults and Jolt's default
  gravity (0, −9.81, 0). Left-handed +Z up (Unreal) or right-handed +Z up (Blender) would need
  axis conversion on every glTF import and fight the library defaults.
- **Clip space.** One engine convention (Y up, depth [0, 1], as Direct3D 12) with a negative
  viewport height on Vulkan (core since Vulkan 1.1, from `VK_KHR_maintenance1`;
  [VkViewport](https://registry.khronos.org/vulkan/specs/latest/man/html/VkViewport.html)), or
  native Vulkan Y-down, which would put a backend detail into every matrix and shader.
- **Depth.** Standard depth wastes float precision near the far plane. Reversed depth with a
  32-bit float buffer spreads precision nearly evenly over distance
  ([Reed, "Depth Precision Visualized", NVIDIA, 2015](https://developer.nvidia.com/content/depth-precision-visualized)),
  and an infinite far plane costs nothing extra
  ([Upchurch and Desbrun, "Tightening the Precision of Perspective Rendering", 2012](https://doi.org/10.1080/2151237X.2012.649987)).
- **World precision.** `float` has a spacing of about 0.24 mm at 2 km and 1 mm at 8 km from the
  origin: enough for the P7 test world. Rendering loses more, because a view-projection matrix
  applied to large world coordinates rounds before the camera offset cancels. Camera-relative
  rendering fixes that for float positions and is required for double positions anyway. Double
  world positions (or origin rebasing) touch the ECS, physics (Jolt needs its
  `JPH_DOUBLE_PRECISION` build) and networking, and are a decision of their own.

## Decision

1. **glm 1.0.3 is the math library** (option A), pinned by URL and SHA-256 like every
   dependency. It lives in a new module **`math`** in the `foundation` layer
   (`engine/ith/math`, headers `"ith/math/…"`), so the `foundation` module keeps its
   standard-library-only rule.
2. **Exception to ADR-0004 rule 5, for glm only.** Public headers of any module may use glm
   types and functions, but only through `ith/math` headers. Engine code never includes
   `<glm/…>` directly; a lint test enforces this. The aliases are `ith::Vec2/3/4`,
   `ith::IVec2/3/4`, `ith::UVec2/3/4`, `ith::Mat3`, `ith::Mat4`, `ith::Quat`, and the double
   types `ith::DVec3` and `ith::DMat4`. A future stable C ABI defines its own plain structs.
3. **glm configuration** is set as public compile definitions of the `math` target, never in a
   header:
   - `GLM_FORCE_DEPTH_ZERO_TO_ONE`: depth [0, 1].
   - `GLM_FORCE_CTOR_INIT`: default-constructed vectors are zero, matrices and quaternions are
     identity.
   - `GLM_FORCE_XYZW_ONLY`: plain `x, y, z, w` members, no anonymous-struct unions (a language
     extension that warns under Clang) and no swizzle proxies; simpler for generated reflection.
   - `GLM_FORCE_QUAT_DATA_XYZW`: the quaternion constructor takes `(x, y, z, w)`, the order of
     its storage, glTF, Jolt and shaders. (Verified in the 1.0.3 source; a unit test pins it.)
   - No `GLM_FORCE_INTRINSICS` and no `GLM_ENABLE_EXPERIMENTAL`. Hot loops (culling, skinning)
     use explicit SIMD in their own modules or run on the GPU.
4. **Spatial conventions:**
   - Right-handed, **+Y up, +X right, forward is −Z**. An identity rotation and a camera look
     down −Z. A glTF asset placed with identity rotation faces +Z, towards the default camera,
     as glTF intends.
   - **Units are SI**: 1 unit = 1 metre, kilograms, seconds. The editor may display other
     units; data and code don't.
   - **Angles are radians** in code and at every API boundary. Degrees exist only in the UI and
     in human-edited files, converted where they are read.
5. **Matrices and rotations:**
   - Column-major storage, column vectors (`v' = M * v`), composition `world = parent * local`,
     `clip = projection * view * world`.
   - Shaders receive matrices unchanged and use `mul(M, v)`; the shader ADR compiles Slang with
     column-major matrix layout.
   - Rotations are unit Hamilton quaternions, `q * v` rotates `v`, and `a * b` applies `b`
     first. Euler angles exist only in the UI and files.
   - The math module provides `Transform` (translation, rotation, scale), bounds, frustum,
     ray tests and the projections below, with tests against known points.
6. **Clip space and depth, identical on every backend:**
   - Clip space is x, y in [−1, 1] with **+Y up**, depth in **[0, 1]**. The Vulkan backend
     uses a negative viewport height. Matrices and shaders are the same on both backends.
   - **Front faces wind counter-clockwise** as the camera sees them (glTF's convention).
   - Scene rendering uses **reversed depth** (near = 1, far = 0) with an **infinite far
     plane**, a 32-bit float depth buffer cleared to 0, and a greater-or-equal test.
     Conventional-depth projections exist for shadow maps.
   - The RHI ADR states the backend settings; a pixel test on both backends checks orientation,
     winding and depth.
7. **Precision:**
   - World positions are `float` (`ith::Vec3`) for now.
   - The renderer is **camera-relative from the start**: per-object transforms have the camera
     position subtracted on the CPU before upload, and the view matrix holds no translation.
   - Serialized positions must not rule out double precision later.
   - Double positions or origin rebasing are decided by a large-world ADR before P7.
8. **Color** math is linear RGB. sRGB exists only in texture and swapchain formats.

## Consequences

- One convention from import to shader output, already proven on both target APIs. glTF needs
  no axis conversion; FBX and other importers convert axes and units on import.
- No math library to write. glm's API becomes part of the engine's public surface: replacing it
  later would be a breaking change, and glm releases are reviewed like engine API changes.
- The configuration macros are global. A third-party library that includes glm with other
  settings would break the one-definition rule, so dependencies are checked for that when
  vetted.
- glm's bounds checks use `assert`, not `ITH_ASSERT`, so they don't reach the engine's assert
  handler.
- Reversed infinite depth needs discipline: every depth clear, test and reconstruction follows
  Decision 6, through shared shader helpers.
- Unreal users find metres and +Y up instead of centimetres and +Z up. The editor can show
  familiar units without changing data.
- Follow-up work: the `math` module with tests (P1), the lint test for direct glm includes, and
  the convention statements in the RHI, shader and large-world ADRs.
- **Revisit** if: glm stops being maintained or 1.1 breaks the configuration (fork or replace
  it behind `ith/math`); profiling shows scalar math is a bottleneck (enable SIMD or add a
  kernel library for hot loops); a console toolchain rejects glm; or precision artifacts appear
  in the large-world benchmark (moves Decision 7 forward).
