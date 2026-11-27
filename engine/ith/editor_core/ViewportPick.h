#pragma once

// Click picking in the viewport (ADR-0042 rules 21 and 22), without any GPU: what an entity writes
// into the viewport's id target, and how a texel read back from it changes the selection.
//
//   // the shapes pass, per draw:
//   const ith::PickTexel texel = ith::pickTexel(entity);     // R = slot index + 1, G = generation
//   // a frame or two after a click, the texel under it read back:
//   ith::applyPick(session, texel, click.mode);              // ith.selection.set with the click's mode
//
// The id target (rhi::Format::R32G32Uint) holds, per pixel, the entity drawn nearest there: R is its
// slot index + 1 and G its generation; R = 0 means no entity (the target is cleared to 0). Its world
// is the editor world, so the world id isn't stored; the generation makes a stale hit checkable.
// Main thread only, like the session.

#include "ith/editor_core/Viewports.h"
#include "ith/foundation/Entity.h"
#include "ith/foundation/Expected.h"

#include <cstdint>

namespace ith {

class EditorSession;

// One texel of the viewport's id target.
struct PickTexel {
    // The entity's slot index + 1; 0: no entity.
    std::uint32_t index = 0;
    // The entity's generation (24 bits).
    std::uint32_t generation = 0;

    friend bool operator==(const PickTexel&, const PickTexel&) = default;
};

// What the shapes pass writes for `entity`. Precondition (asserted): the slot index is below the
// largest uint32, so its texel can't read as no entity; the null handle gives the empty texel.
[[nodiscard]] PickTexel pickTexel(Entity entity) noexcept;

// The entity of the world with id `worldId` that `texel` names; the null handle for the empty texel
// (index 0).
[[nodiscard]] Entity pickedEntity(PickTexel texel, std::uint8_t worldId) noexcept;

// Applies a read-back click to the session's selection through ith.selection.set (ADR-0042
// rule 22). Empty space (index 0) clears the selection for PickMode::Replace and changes nothing for
// Toggle and Add. A hit on an entity of the document's world replaces, toggles or adds its stable
// id. A stale hit (the entity was despawned since the frame was drawn) and a hit on an entity
// without a StableId change nothing (ADR-0037 rule 5). Fails only with the command's error.
Expected<void> applyPick(EditorSession& session, PickTexel texel, PickMode mode);

} // namespace ith
