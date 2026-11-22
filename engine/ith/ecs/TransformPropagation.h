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
// Slice 5 propagates every entity each time. Propagating only the subtrees whose local transforms
// or parent links changed comes with change ticks (ADR-0014 slice 6). Tools that need a world
// matrix before the next propagation compute it from the local transforms.

#include "ith/ecs/Schedule.h"
#include "ith/ecs/World.h"

namespace ith {

// Writes every WorldTransform of the world from the local transforms.
void propagateTransforms(World& world);

// The system "ith::propagate_transforms" in the PostUpdate stage, which propagates the
// transforms of the world it runs on. It reads LocalTransform and Children and writes
// WorldTransform.
[[nodiscard]] SystemDescriptor transformPropagationSystem();

} // namespace ith
