#pragma once

// Copying a whole World into a fresh one (ADR-0014 rule 4, ADR-0032), as Play In Editor does
// with the editor world (ADR-0017 rule 10).
//
//   ith::World play;
//   ith::copyWorld(editorWorld, play, {.excludeEditorOnly = true});
//
// What the copy holds:
// - Every live entity in the same slot, with the same generation, under the target's world id:
//   an entity `e` of the source is Entity::fromParts(e.index(), e.generation(), target.id()) in the
//   copy. The free slots are the same too. Match entities across worlds by StableId, never by
//   handle: comparing handles of two worlds is a bug that the world id check catches.
// - Every component value, Transient ones included, copied through its reflection record. Every
//   reflected Entity in it is remapped to the copy's entity (ith::remapEntities); a handle that
//   isn't alive in the source (stale, null, of another world) becomes the null handle. An Entity
//   in an unreflected field is copied as it is and still points into the source.
// - The hierarchy as it is: parents, children in order, the order of the roots, Disabled and
//   DisabledInHierarchy, and the stable-id lookup.
// - Every resource, copied and remapped the same way.
// - With excludeEditorOnly, no component or resource whose type is EditorOnly, so the copy looks
//   like what the cooker produces.
//
// What it doesn't:
// - No hooks run: the source's invariants are copied, not rebuilt through hooks.
// - Change ticks start over: the target's clock is untouched and every value is stamped added and
//   changed at its current tick, as after a level load. The source's system runs are not copied.
// - Spawns the source reserved but hasn't applied are not copied.

#include "ith/ecs/World.h"

namespace ith {

struct WorldCopyOptions {
    // Leaves out every component and resource whose type is EditorOnly (ADR-0017).
    bool excludeEditorOnly = false;
};

// Copies `source` into `target` (see the header comment) and returns true. Returns false,
// asserting and changing nothing, when `target` is `source`, when `source` is being iterated or
// runs a hook, or when `target` isn't fresh: it must be as its constructor left it, with nothing
// ever spawned or reserved, no resources and no system run.
bool copyWorld(const World& source, World& target, const WorldCopyOptions& options = {});

} // namespace ith
