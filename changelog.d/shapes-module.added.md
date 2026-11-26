- `ith/shapes`: primitive shapes for the editor's preview (ADR-0042 rule 19). `ith::ShapeRenderer
  { shape, color }` is a reflected runtime component (a Cube, Sphere, Plane or Cylinder and a linear
  RGB colour) that levels save and load like any component. `ith::shapeMesh` gives each shape's
  unit mesh (positions and normals, 16-bit triangle-list indices, counter-clockwise front faces seen
  from outside) and `ith::shapeBounds` its local bounds: a 1 m cube, a 0.5 m radius sphere, a 1 m
  square plane on y = 0 facing +Y, and a 0.5 m radius, 1 m tall cylinder along Y, all centred on
  the origin.
