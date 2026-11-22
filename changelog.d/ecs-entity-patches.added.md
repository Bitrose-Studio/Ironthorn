- `ith::World::applyPatches` applies component patches, adds and removes to several entities as
  one step (`ith/ecs/EntityPatch.h`: `ith::EntityPatch`, `ith::ComponentChange`). Everything is
  checked first, so a failure changes nothing; a patch of a mutable component is written in place
  without hooks, an immutable one is replaced through `add` with its hooks, and the call returns
  the patches that undo the step.
