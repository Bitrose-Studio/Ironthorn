#pragma once

// Transform propagation (ADR-0014 rule 13, ADR-0031 rule 2): every WorldTransform from the local
// transforms down the hierarchy.
//
//   schedule.add(ith::transformPropagationSystem()); // in PostUpdate
//   // or, outside a schedule:
//   ith::propagateTransforms(world);
//
// Each root's WorldTransform is its local transform, and each child's is its parent's world
// matrix times its local transform (world = parent * local, ADR-0006). An entity without a
// LocalTransform counts as the identity; one without a WorldTransform is skipped, but its children
// still get theirs. Disabled entities are propagated too.
//
// Only the subtrees that changed since the last propagation are written (ADR-0014 rule 13, with
// the change ticks of World.h): those below an entity whose LocalTransform was added, written or
// removed, whose WorldTransform was added or written elsewhere, or whose ChildOf was added,
// replaced or removed. The first propagation on a world writes everything. The matrices are the
// same as a full propagation would give. Each WorldTransform written is marked changed. The
// system and direct calls each keep their own last run, per world. Tools that need a world matrix
// before the next propagation compute it from the local transforms.

#include "ith/ecs/Schedule.h"
#include "ith/ecs/World.h"

namespace ith {

// Writes the WorldTransforms that changes since this function's last call on `world` made stale.
// Counts as a run of its own for change detection (World.h).
void propagateTransforms(World& world);

// The system "ith::propagate_transforms" in the PostUpdate stage, which propagates the
// transforms of the world it runs on that changed since its last run there. It reads
// LocalTransform, ChildOf and Children and writes WorldTransform.
[[nodiscard]] SystemDescriptor transformPropagationSystem();

} // namespace ith
