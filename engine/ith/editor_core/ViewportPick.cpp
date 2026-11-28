#include "ith/editor_core/ViewportPick.h"

#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/EditorDocument.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/foundation/Assert.h"
#include "ith/serialization/Value.h"

#include <limits>
#include <string>
#include <utility>

namespace ith {

namespace {

using serialization::Value;

const char* modeName(PickMode mode) noexcept {
    switch (mode) {
        case PickMode::Toggle:
            return "toggle";
        case PickMode::Add:
            return "add";
        case PickMode::Replace:
            break;
    }
    return "replace";
}

} // namespace

PickTexel pickTexel(Entity entity) noexcept {
    if (entity.isNull()) {
        return {};
    }
    ITH_ASSERT_MSG(entity.index() < std::numeric_limits<std::uint32_t>::max(),
                   "an entity's slot index + 1 fits the id target's 32 bits");
    return {.index = entity.index() + 1, .generation = entity.generation()};
}

Entity pickedEntity(PickTexel texel, std::uint8_t worldId) noexcept {
    if (texel.index == 0) {
        return {};
    }
    return Entity::fromParts(texel.index - 1, texel.generation, worldId);
}

Expected<void> applyPick(EditorSession& session, std::size_t viewport, PickTexel texel, PickMode mode) {
    ITH_ASSERT_MSG(viewport < kMaxViewports, "there are only four viewports");
    Value entities = Value::array();
    if (texel.index == 0) {
        // Empty space: a plain click clears the selection; Ctrl and Shift keep it.
        if (mode != PickMode::Replace) {
            return {};
        }
    } else {
        // The texel names an entity of the world this viewport draws (ADR-0053 decision 16).
        const World& world = session.drawnWorld(viewport < kMaxViewports ? viewport : 0);
        const Entity entity = pickedEntity(texel, world.id());
        if (!world.isAlive(entity)) {
            return {}; // despawned since the frame was drawn
        }
        const auto* id = world.get<const StableId>(entity);
        if (id == nullptr) {
            return {}; // only entities with a stable id are selectable (ADR-0037 rule 5)
        }
        if (session.active().world.findByStableId(id->id).isNull()) {
            return {}; // an editor entity the game world has despawned: nothing to select there
        }
        entities.elements().push_back(Value::string(id->id.toString()));
    }
    Value args = Value::object();
    args.set("entities", std::move(entities));
    args.set("mode", Value::string(modeName(mode)));
    if (Expected<Value> result = session.execute("ith.selection.set", args); !result) {
        return makeUnexpected(std::move(result).error());
    }
    return {};
}

} // namespace ith
