- `ith/math`: the engine's math types, glm 1.0.3 under engine names (`ith::Vec2/3/4`,
  `IVec2/3/4`, `UVec2/3/4`, `Mat3`, `Mat4`, `Quat`, `DVec3`, `DMat4`), with zero and identity
  defaults, `Quat(x, y, z, w)` and depth in [0, 1]. glm's functions come through
  `ith/math/Math.h`; including `<glm/...>` anywhere else fails the `glm_includes` test. See
  `docs/development/math.md` for the conventions: right-handed, +Y up, −Z forward, metres,
  radians, column vectors.
- `ith::perspectiveReversedInfinite`: the scene camera's projection, with reversed depth (1 at
  the near plane, towards 0 far away) and no far plane.
- `ith::Transform` (translation, rotation, scale), and `Aabb`, `Sphere`, `Plane`, `Frustum` and
  `Ray` with frustum culling, `transformBounds` and ray tests.
- Reflection describes the math types, so they can be fields of reflected types and components
  and are saved by both archives (a `Vec3` as `{"x": 1, "y": 2, "z": 3}`). A new `TypeRegistry`
  holds them.
