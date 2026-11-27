// The Viewport commands of editor_core (ADR-0042 rules 20 and 26 to 29): the transform tools, Focus,
// Snap to Floor, the gizmo space, snapping, the game view and immersive mode.
//
// They change the Viewports store, which is view state, so they are Actions; only Snap to Floor
// changes the world, as an Edit command in one transaction. World bounds are a ShapeRenderer's
// shape bounds moved by World::worldMatrixOf: the matrix WorldTransform holds once transforms are
// propagated, computed at once, so the commands don't wait for propagation.

#include "ith/ecs/EntityPatch.h"
#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/Query.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/CommandArgs.h"
#include "ith/editor_core/EditorCamera.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Viewports.h"
#include "ith/math/Geometry.h"
#include "ith/reflection/OwnedValue.h"
#include "ith/reflection/Patch.h"
#include "ith/reflection/PropertyPath.h"
#include "ith/reflection/TypeOf.h"
#include "ith/serialization/TextArchive.h"
#include "ith/shapes/ShapeMesh.h"
#include "ith/shapes/ShapeRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ith {

namespace {

using detail::childrenOf;
using detail::hasAncestorIn;
using detail::invalidArgs;
using detail::optionalString;
using detail::targets;
using serialization::Value;

// How far above a support an entity's bottom may be and still rest on it, in metres (ADR-0042
// rule 28's small tolerance).
constexpr float kFloorTolerance = 0.001f;

// A drop shorter than this, in metres, means the entity already rests: rounding in the world
// matrices of rotated or scaled parents leaves such residues, and they would make empty undo
// entries.
constexpr float kRestingEpsilon = 1e-5f;

// A parent whose matrix has a smaller 3x3 determinant collapses a direction (a zero scale), so a
// world-space drop has no local translation that gives it.
constexpr float kSingularDeterminant = 1e-12f;

// The world bounds of the entity's shape; nullopt for an entity without a ShapeRenderer.
std::optional<Aabb> shapeWorldBounds(const World& world, Entity entity) {
    const auto* renderer = world.get<const ShapeRenderer>(entity);
    if (renderer == nullptr) {
        return std::nullopt;
    }
    return transformBounds(shapeBounds(renderer->shape), world.worldMatrixOf(entity));
}

Aabb unite(const Aabb& a, const Aabb& b) {
    return Aabb{.min = glm::min(a.min, b.min), .max = glm::max(a.max, b.max)};
}

// The viewport the "viewport" argument names (kDefaultViewport when it is left out) and its id.
struct NamedViewport {
    ViewportState* state = nullptr;
    std::string_view instance;
};

Expected<NamedViewport> viewportArg(EditorSession& session, std::string_view command, const Value& args) {
    Expected<std::optional<std::string_view>> name = optionalString(command, args, "viewport");
    if (!name) {
        return makeUnexpected(std::move(name).error());
    }
    const std::string_view instance = name->value_or(kDefaultViewport);
    ViewportState* viewport = session.viewports().find(instance);
    if (viewport == nullptr) {
        return invalidArgs(command,
                           R"("viewport" must be a viewport instance id, "ith.viewport.1" to "ith.viewport.4")");
    }
    return NamedViewport{.state = viewport, .instance = instance};
}

// --- Tools ---

std::string_view toolName(ViewportTool tool) {
    switch (tool) {
        case ViewportTool::Translate:
            return "translate";
        case ViewportTool::Rotate:
            return "rotate";
        case ViewportTool::Scale:
            return "scale";
    }
    return "translate";
}

Value toolResult(const EditorSession& session) {
    Value result = Value::object();
    result.set("tool", Value::string(std::string(toolName(session.viewports().tool()))));
    return result;
}

void addToolCommand(CommandRegistry& registry, std::string id, std::string displayName, std::string tooltip,
                    ViewportTool tool) {
    registry.add({.id = std::move(id),
                  .displayName = std::move(displayName),
                  .tooltip = std::move(tooltip),
                  .category = "Viewport",
                  .kind = CommandKind::Action,
                  .execute = [tool](EditorSession& session, const Value&) -> Expected<Value> {
                      session.viewports().setTool(tool);
                      return toolResult(session);
                  }});
}

Expected<Value> cycleTool(EditorSession& session, const Value& /*args*/) {
    Viewports& viewports = session.viewports();
    switch (viewports.tool()) {
        case ViewportTool::Translate:
            viewports.setTool(ViewportTool::Rotate);
            break;
        case ViewportTool::Rotate:
            viewports.setTool(ViewportTool::Scale);
            break;
        case ViewportTool::Scale:
            viewports.setTool(ViewportTool::Translate);
            break;
    }
    return toolResult(session);
}

// --- Focus ---

Expected<void> somethingSelected(EditorSession& session) {
    if (session.document().selection().empty()) {
        return makeError("editor.selection.empty", "Select something to focus on");
    }
    return {};
}

// Frames the union of the selected entities' world bounds; an entity without a shape adds its
// position.
Expected<Value> focusSelection(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.viewport.focus_selection";
    Expected<NamedViewport> viewport = viewportArg(session, kCommand, args);
    if (!viewport) {
        return makeUnexpected(std::move(viewport).error());
    }
    const World& world = session.document().world();
    std::optional<Aabb> bounds;
    for (const Uuid& id : session.document().selection().entities()) {
        const Entity entity = world.findByStableId(id);
        if (entity.isNull()) {
            continue;
        }
        const std::optional<Aabb> shape = shapeWorldBounds(world, entity);
        const Vec3 position(world.worldMatrixOf(entity)[3]);
        const Aabb entityBounds = shape.value_or(Aabb{.min = position, .max = position});
        bounds = bounds.has_value() ? unite(*bounds, entityBounds) : entityBounds;
    }
    if (!bounds.has_value()) {
        return makeError("editor.selection.empty", "Select something to focus on");
    }
    focus(viewport->state->camera, *bounds, viewport->state->aspect());
    return Value();
}

// --- Snap to Floor ---

// Whether the boxes' ranges on the ground (X and Z) overlap with an area.
bool overlapOnGround(const Aabb& a, const Aabb& b) {
    return a.min.x < b.max.x && b.min.x < a.max.x && a.min.z < b.max.z && b.min.z < a.max.z;
}

// Adds `entity` and everything below it to `subtree`.
void collectSubtree(const World& world, Entity entity, std::unordered_set<std::uint64_t>& subtree) {
    subtree.insert(entity.bits());
    if (const std::vector<Entity>* children = childrenOf(world, entity)) {
        for (const Entity child : *children) {
            collectSubtree(world, child, subtree);
        }
    }
}

// The height `bottom` comes to rest at: the highest support at or below it (with the tolerance),
// or the highest support when it hangs below every one. The ground (y = 0) is always a support.
float restingHeight(float bottom, const Aabb& bounds, const std::vector<Aabb>& supports) {
    float highest = 0.0f;
    std::optional<float> below;
    if (0.0f <= bottom + kFloorTolerance) {
        below = 0.0f;
    }
    for (const Aabb& support : supports) {
        if (!overlapOnGround(bounds, support)) {
            continue;
        }
        const float top = support.max.y;
        highest = std::max(highest, top);
        if (top <= bottom + kFloorTolerance) {
            below = std::max(below.value_or(top), top);
        }
    }
    return below.value_or(highest);
}

Expected<Value> snapToFloor(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.viewport.snap_to_floor";
    Expected<std::vector<Entity>> entities = targets(session, kCommand, args);
    if (!entities) {
        return makeUnexpected(std::move(entities).error());
    }
    // Mutable only for the support query below, which reads.
    World& world = session.document().mutableWorld();
    std::unordered_set<std::uint64_t> selected;
    for (const Entity entity : *entities) {
        selected.insert(entity.bits());
    }
    std::unordered_set<std::uint64_t> moved;
    std::vector<Entity> roots;
    for (const Entity entity : *entities) {
        if (!hasAncestorIn(world, entity, selected)) {
            roots.push_back(entity);
            collectSubtree(world, entity, moved);
        }
    }
    // Enabled shapes outside the moved subtrees; a disabled entity is never a support.
    std::vector<Aabb> supports;
    world.query<const ShapeRenderer>().forEach([&](Entity entity, const ShapeRenderer& renderer) {
        if (!moved.contains(entity.bits())) {
            supports.push_back(transformBounds(shapeBounds(renderer.shape), world.worldMatrixOf(entity)));
        }
    });
    Expected<PropertyPath> translation = PropertyPath::parse(typeOf<LocalTransform>(), "value.translation");
    if (!translation) {
        return makeUnexpected(std::move(translation).error());
    }
    std::vector<EntityPatch> patches;
    for (const Entity root : roots) {
        const std::optional<Aabb> bounds = shapeWorldBounds(world, root);
        const auto* local = world.get<const LocalTransform>(root);
        if (!bounds.has_value() || local == nullptr) {
            continue;
        }
        const float drop = restingHeight(bounds->min.y, *bounds, supports) - bounds->min.y;
        if (std::abs(drop) < kRestingEpsilon) {
            continue;
        }
        // The world-space drop in the parent's space, where the local translation lives. A parent
        // that collapses a direction (a zero scale) can't move it there: the root is skipped.
        const auto* link = world.get<const ChildOf>(root);
        const Mat4 parent = link != nullptr ? world.worldMatrixOf(link->parent) : Mat4(1.0f);
        if (std::abs(glm::determinant(Mat3(parent))) < kSingularDeterminant) {
            continue;
        }
        const Vec3 offset(glm::inverse(parent) * Vec4(0.0f, drop, 0.0f, 0.0f));
        if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z)) {
            continue;
        }
        Patch patch(typeOf<LocalTransform>());
        patch.set(*translation, OwnedValue::of(local->value.translation + offset));
        EntityPatch& entityPatch = patches.emplace_back(EntityPatch{.entity = root});
        entityPatch.changes.push_back(ComponentChange::makePatch(std::move(patch)));
    }
    return detail::applyAll(session, patches);
}

// --- Gizmo space, snapping, game view, immersive ---

Expected<Value> toggleGizmoSpace(EditorSession& session, const Value& /*args*/) {
    Viewports& viewports = session.viewports();
    viewports.setGizmoSpace(viewports.gizmoSpace() == GizmoSpace::World ? GizmoSpace::Local : GizmoSpace::World);
    Value result = Value::object();
    result.set("space", Value::string(viewports.gizmoSpace() == GizmoSpace::World ? "world" : "local"));
    return result;
}

// "enabled" (true or false) and "step" (metres, finite and positive), each optional: what is left
// out stays.
Expected<Value> setSnap(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.viewport.set_snap";
    Viewports& viewports = session.viewports();
    const Value* enabled = args.kind() == Value::Kind::Object ? args.find("enabled") : nullptr;
    if (enabled != nullptr && enabled->kind() != Value::Kind::Bool) {
        return invalidArgs(kCommand, "\"enabled\" must be true or false");
    }
    const Value* step = args.kind() == Value::Kind::Object ? args.find("step") : nullptr;
    std::optional<float> stepValue;
    if (step != nullptr) {
        stepValue = step->kind() == Value::Kind::Number ? step->toNumber<float>() : std::nullopt;
        if (!stepValue.has_value() || !std::isfinite(*stepValue) || *stepValue <= 0.0f) {
            return invalidArgs(kCommand, "\"step\" must be a positive number of metres");
        }
    }
    viewports.setSnap(enabled != nullptr ? enabled->asBool() : viewports.snapEnabled(),
                      stepValue.value_or(viewports.snapStep()));
    Value result = Value::object();
    result.set("enabled", Value::boolean(viewports.snapEnabled()));
    // The shortest number that reads back as the same float, so 0.1f is 0.1.
    const float snapStep = viewports.snapStep();
    Expected<Value, serialization::Error> stepText = serialization::toValue(typeOf<float>(), &snapStep);
    if (!stepText) {
        return makeError(stepText.error().code, stepText.error().toString());
    }
    result.set("step", std::move(*stepText));
    return result;
}

Expected<Value> toggleGameView(EditorSession& session, const Value& args) {
    Expected<NamedViewport> viewport = viewportArg(session, "ith.viewport.game_view", args);
    if (!viewport) {
        return makeUnexpected(std::move(viewport).error());
    }
    viewport->state->gameView = !viewport->state->gameView;
    Value result = Value::object();
    result.set("viewport", Value::string(std::string(viewport->instance)));
    result.set("gameView", Value::boolean(viewport->state->gameView));
    return result;
}

Expected<void> notInP2(EditorSession& /*session*/) {
    return makeError("editor.viewport.not_available", "Immersive mode is not in P2");
}

} // namespace

void registerViewportCommands(CommandRegistry& registry) {
    addToolCommand(registry, "ith.viewport.translate_tool", "Translate", "Move the selection with the gizmo",
                   ViewportTool::Translate);
    addToolCommand(registry, "ith.viewport.rotate_tool", "Rotate", "Rotate the selection with the gizmo",
                   ViewportTool::Rotate);
    addToolCommand(registry, "ith.viewport.scale_tool", "Scale", "Scale the selection with the gizmo",
                   ViewportTool::Scale);
    registry.add({.id = "ith.viewport.cycle_tool",
                  .displayName = "Cycle Transform Tool",
                  .tooltip = "Switch from translate to rotate to scale and back",
                  .category = "Viewport",
                  .kind = CommandKind::Action,
                  .execute = &cycleTool});
    registry.add({.id = "ith.viewport.focus_selection",
                  .displayName = "Focus Selected",
                  .tooltip = "Frame the selection in the viewport",
                  .category = "Viewport",
                  .kind = CommandKind::Action,
                  .canExecute = &somethingSelected,
                  .execute = &focusSelection});
    registry.add({.id = "ith.viewport.snap_to_floor",
                  .displayName = "Snap to Floor",
                  .tooltip = "Drop the selection onto the ground or the shape below it",
                  .category = "Edit",
                  .execute = &snapToFloor});
    registry.add({.id = "ith.viewport.toggle_gizmo_space",
                  .displayName = "Toggle World/Local",
                  .tooltip = "Align the gizmo with the world or with the selection",
                  .category = "Viewport",
                  .kind = CommandKind::Action,
                  .execute = &toggleGizmoSpace});
    registry.add({.id = "ith.viewport.set_snap",
                  .displayName = "Snapping",
                  .tooltip = "Turn snapping on or off and set its step",
                  .category = "Viewport",
                  .kind = CommandKind::Action,
                  .execute = &setSnap});
    registry.add({.id = "ith.viewport.game_view",
                  .displayName = "Game View",
                  .tooltip = "Show what the game would see: no grid, gizmo or selection",
                  .category = "Viewport",
                  .kind = CommandKind::Action,
                  .execute = &toggleGameView});
    registry.add({.id = "ith.viewport.immersive",
                  .displayName = "Immersive Mode",
                  .tooltip = "Hide the editor around the viewport",
                  .category = "Viewport",
                  .kind = CommandKind::Action,
                  .canExecute = &notInP2,
                  .execute = [](EditorSession& session, const Value&) -> Expected<Value> {
                      return makeUnexpected(notInP2(session).error());
                  }});
}

} // namespace ith
