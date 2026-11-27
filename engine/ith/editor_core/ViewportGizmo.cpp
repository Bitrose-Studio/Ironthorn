#include "ith/editor_core/ViewportGizmo.h"

#include "ith/ecs/EntityPatch.h"
#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/CommandArgs.h"
#include "ith/editor_core/CommandRegistry.h"
#include "ith/editor_core/EditorDocument.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Selection.h"
#include "ith/editor_core/Viewports.h"
#include "ith/reflection/OwnedValue.h"
#include "ith/reflection/Patch.h"
#include "ith/reflection/TypeOf.h"
#include "ith/serialization/Value.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ith {

namespace {

constexpr std::string_view kDuplicate = "ith.edit.duplicate";

// A parent matrix whose 3x3, its columns made unit length, has a smaller determinant collapses a
// direction (a zero scale, or columns nearly parallel): no local translation gives a world-space move
// there. Scale-invariant, so a tiny but uniform scale still moves.
constexpr double kSingularDeterminant = 1e-6;

// The inverse of the parent's 3x3, world-space directions to its space; std::nullopt when it
// collapses a direction or the inverse isn't finite. In double, so a tiny scale's determinant
// doesn't underflow.
std::optional<Mat3> fromWorldOf(const Mat3& parent) noexcept {
    const glm::dmat3 matrix(parent);
    const double lengths = glm::length(matrix[0]) * glm::length(matrix[1]) * glm::length(matrix[2]);
    if (!(lengths > 0.0) || !(std::abs(glm::determinant(matrix)) / lengths >= kSingularDeterminant)) {
        return std::nullopt;
    }
    const Mat3 inverse(glm::inverse(matrix));
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            if (!std::isfinite(inverse[column][row])) {
                return std::nullopt;
            }
        }
    }
    return inverse;
}

bool finite(const Vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

} // namespace

std::optional<ViewportGizmo> viewportGizmo(const EditorSession& session, const ViewportState& state,
                                           float displayScale) {
    const EditorDocument& document = session.document();
    if (session.viewports().tool() != ViewportTool::Translate || state.gameView || document.playing() ||
        document.opening() || !hasImageArea(state.imageSize)) {
        return std::nullopt;
    }
    // Another interaction (a Details field being typed into, say) would refuse a drag: no handle is
    // offered. A gizmo drag's own transaction keeps it shown.
    if (document.transactions().isOpen()) {
        bool gizmoDrag = false;
        for (std::size_t index = 0; index < kMaxViewports; ++index) {
            gizmoDrag = gizmoDrag || session.viewports().at(index).gizmoDrag != GizmoHandle::None;
        }
        if (!gizmoDrag) {
            return std::nullopt;
        }
    }
    const std::optional<Uuid> primary = document.selection().primary();
    if (!primary.has_value()) {
        return std::nullopt;
    }
    const World& world = document.world();
    const Entity entity = world.findByStableId(*primary);
    if (entity.isNull()) {
        return std::nullopt;
    }
    const GizmoFrame frame = gizmoFrame(world.worldMatrixOf(entity), session.viewports().gizmoSpace());
    const std::optional<GizmoLayout> layout =
        layoutGizmo(state.camera, state.imageSize, frame, kGizmoArrowLength * displayScale);
    if (!layout.has_value()) {
        return std::nullopt;
    }
    return ViewportGizmo{.frame = frame, .layout = *layout};
}

GizmoDrag::GizmoDrag(EditorSession& session, TransactionScope scope, const GizmoGrab& grab, PropertyPath translation,
                     std::vector<Root> roots)
    : m_session(&session),
      m_scope(std::move(scope)),
      m_grab(grab),
      m_translation(std::move(translation)),
      m_roots(std::move(roots)) {}

Expected<GizmoDrag> GizmoDrag::begin(EditorSession& session, GizmoHandle handle, const GizmoFrame& frame,
                                     const Ray& ray, bool duplicate) {
    EditorDocument& document = session.document();
    if (document.transactions().isOpen()) {
        return makeError("editor.command.interaction_open", "an interaction is in progress");
    }
    if (document.playing()) {
        return makeError("editor.command.playing", "the level can't change while playing");
    }
    if (document.opening()) {
        return makeError("editor.document.busy", "a level is being opened");
    }
    if (document.selection().empty()) {
        return makeError("editor.selection.empty", "Select something to move");
    }
    const std::optional<GizmoGrab> grab = grabGizmo(handle, frame, ray);
    if (!grab.has_value()) {
        return makeError("editor.gizmo.no_grab", "the press grabs no point of the gizmo's handle");
    }
    Expected<PropertyPath> translation = PropertyPath::parse(typeOf<LocalTransform>(), "value.translation");
    if (!translation) {
        return makeUnexpected(std::move(translation).error());
    }
    const CommandDesc* duplicateCommand = nullptr;
    if (duplicate) {
        if (Expected<void> allowed = session.commands().canExecute(session, kDuplicate); !allowed) {
            return makeUnexpected(std::move(allowed).error());
        }
        duplicateCommand = session.commands().find(kDuplicate);
    }

    TransactionScope scope = document.transactions().begin("Translate");
    if (duplicateCommand != nullptr) {
        // Inside the open transaction, which the command's changes join: the copies become the
        // selection, and one undo entry holds both the copies and their move. The registry refuses
        // to run a command while a transaction is open, so its function runs directly, after the
        // registry's checks above. Invariant: that function must not run registry commands itself.
        // It runs outside the registry's depth counter here, so a nested command would be refused
        // as an open interaction (see duplicate() in EditCommands.cpp).
        if (Expected<serialization::Value> copied = duplicateCommand->execute(session, serialization::Value());
            !copied) {
            scope.cancel();
            return makeUnexpected(std::move(copied).error());
        }
    }

    // The selected roots: a selected entity whose ancestor is selected moves with it.
    const World& world = document.world();
    std::vector<Entity> selected;
    std::unordered_set<std::uint64_t> bits;
    for (const Uuid& id : document.selection().entities()) {
        const Entity entity = world.findByStableId(id);
        if (!entity.isNull()) {
            selected.push_back(entity);
            bits.insert(entity.bits());
        }
    }
    std::vector<Root> roots;
    std::size_t singular = 0;
    for (const Entity entity : selected) {
        const auto* local = world.get<const LocalTransform>(entity);
        if (local == nullptr || detail::hasAncestorIn(world, entity, bits)) {
            continue;
        }
        const auto* link = world.get<const ChildOf>(entity);
        const std::optional<Mat3> fromWorld =
            link != nullptr ? fromWorldOf(Mat3(world.worldMatrixOf(link->parent))) : std::optional<Mat3>(Mat3(1.0f));
        if (!fromWorld.has_value()) {
            ++singular;
            continue;
        }
        roots.push_back(Root{.entity = entity, .translation = local->value.translation, .fromWorld = *fromWorld});
    }
    if (singular > 0) {
        ITH_LOG_INFO(Gizmo, "{} selected entities can't move: a parent collapses a direction (a zero scale)", singular);
    }
    return GizmoDrag(session, std::move(scope), *grab, std::move(*translation), std::move(roots));
}

Expected<void> GizmoDrag::preview(const Ray& ray) {
    if (!m_scope.active() || m_roots.empty()) {
        return {}; // Esc cancelled the drag (it waits for its release), or nothing can move
    }
    const std::optional<Vec3> dragged = dragGizmo(m_grab, ray);
    if (!dragged.has_value()) {
        return {};
    }
    Vec3 delta = *dragged;
    const Viewports& viewports = m_session->viewports();
    if (viewports.snapEnabled()) {
        delta = snapGizmo(delta, m_grab.frame, viewports.snapStep());
    }
    if (delta == m_delta) {
        return {};
    }
    std::vector<EntityPatch> patches;
    patches.reserve(m_roots.size());
    for (const Root& root : m_roots) {
        const Vec3 translation = root.translation + root.fromWorld * delta;
        if (!finite(root.translation) || !finite(translation)) {
            return {}; // the whole frame, so the roots never move apart: the last position stays
        }
        Patch patch(typeOf<LocalTransform>());
        patch.set(m_translation, OwnedValue::of(translation));
        EntityPatch& entityPatch = patches.emplace_back(EntityPatch{.entity = root.entity});
        entityPatch.changes.push_back(ComponentChange::makePatch(std::move(patch)));
    }
    if (Expected<void> applied = m_scope.apply(patches); !applied) {
        return applied;
    }
    m_delta = delta;
    return {};
}

void GizmoDrag::commit() {
    // Nothing moved (no non-zero delta is applied): an Alt press leaves no copies behind.
    if (m_delta == Vec3(0.0f)) {
        m_scope.cancel();
        return;
    }
    m_scope.commit();
}

void GizmoDrag::cancel() {
    m_scope.cancel();
}

} // namespace ith
