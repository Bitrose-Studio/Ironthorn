#pragma once

// The translate gizmo of a viewport, from the session (ADR-0042 rules 24 to 26): whether it shows,
// where, and its drag as one transaction with a live preview (ADR-0037 rule 6, the Details pattern).
// The math is Gizmo.h's; the drawing is the editor UI's.
//
//   const float scale = window.displayScale();
//   if (const std::optional<ith::ViewportGizmo> gizmo = ith::viewportGizmo(session, state, scale)) {
//       const ith::GizmoHandle handle = ith::hitGizmo(gizmo->layout, pixel, ith::kGizmoGrabTolerance * scale);
//       ith::Expected<ith::GizmoDrag> drag = ith::GizmoDrag::begin(session, handle, gizmo->frame,
//                                                                  ith::pickRay(state.camera, state.imageSize, pixel),
//                                                                  /*duplicate=*/alt);
//       drag->preview(ray);  // whenever the pointer moved
//       drag->commit();      // on the release: one undo entry, "Translate"
//   }
//
// The viewport consumer (ViewportInput.h) runs the drag; tests may drive it directly. Main thread
// only, like the session.

#include "ith/editor_core/Gizmo.h"
#include "ith/editor_core/Transactions.h"
#include "ith/foundation/Entity.h"
#include "ith/foundation/Expected.h"
#include "ith/foundation/Log.h"
#include "ith/math/Geometry.h"
#include "ith/math/Types.h"
#include "ith/reflection/PropertyPath.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace ith {

class EditorSession;
struct ViewportState;

// The translate gizmo's log category: drags that can't begin, refused previews, roots that can't move.
ITH_LOG_CATEGORY(Gizmo);

// The gizmo a viewport shows: its frame in the world and its handles on the image.
struct ViewportGizmo {
    GizmoFrame frame;
    GizmoLayout layout;
};

// The gizmo of the viewport `state` at `displayScale` (the window's display scale: arrows are
// kGizmoArrowLength times it in pixels), or std::nullopt when none shows: the tool isn't Translate,
// the viewport is in game view, its image has no area, the level is playing or being opened, nothing
// is selected, the primary selected entity isn't live, the primary is not in front of the camera, or
// another interaction's transaction is open (a Details field being typed into: a drag would be
// refused, so no handle is offered; a gizmo drag's own transaction, while any ViewportState::gizmoDrag
// names a handle, keeps it shown).
// The gizmo sits at the primary's world position (World::worldMatrixOf, so it follows a drag at
// once), with the world's axes or the primary's rotation (Viewports::gizmoSpace()). The camera and
// the image are the state's: what the panel last reported.
[[nodiscard]] std::optional<ViewportGizmo> viewportGizmo(const EditorSession& session, const ViewportState& state,
                                                         float displayScale);

// One gizmo drag: one transaction named "Translate", from the press to the release.
class GizmoDrag {
public:
    // Starts dragging `handle` of the gizmo at `frame` with the press's ray: opens the transaction
    // and, with `duplicate` (Alt at the press), first duplicates the selection inside it (as
    // ith.edit.duplicate does, whose copies become the selection), so the copies are dragged and Esc
    // or cancel() removes them again. The dragged entities are the selected roots: selected entities
    // without a selected ancestor, which have a LocalTransform and whose parent's matrix doesn't
    // collapse a direction (scale-invariant: the determinant of its 3x3 with unit columns is at least
    // 1e-6, so a tiny uniform scale still moves); their children follow. Roots skipped for such a
    // parent are logged once per drag (Info, "Gizmo"). Fails, with nothing changed, with
    // "editor.command.interaction_open" (a transaction is open), "editor.command.playing",
    // "editor.document.busy" (a level is being opened), "editor.selection.empty", "editor.gizmo.no_grab"
    // (the ray grabs no point of the handle: grabGizmo) and, with `duplicate`, the duplicate
    // command's errors.
    [[nodiscard]] static Expected<GizmoDrag> begin(EditorSession& session, GizmoHandle handle, const GizmoFrame& frame,
                                                   const Ray& ray, bool duplicate);

    GizmoDrag(const GizmoDrag&) = delete;
    GizmoDrag& operator=(const GizmoDrag&) = delete;
    GizmoDrag(GizmoDrag&&) noexcept = default;
    GizmoDrag& operator=(GizmoDrag&&) noexcept = default;
    // Cancels the transaction unless commit() or cancel() ended it.
    ~GizmoDrag() = default;

    // Moves the dragged entities to where the pointer's `ray` drags the handle (dragGizmo), snapped
    // per gizmo axis when the Viewports store's snapping is on (its step read now): each root's local
    // translation becomes its translation at the press plus the world-space delta in its parent's
    // space, so it moves the same in the world under rotated and scaled parents. A ray that grabs no
    // point keeps the last position; a delta equal to the last applied one applies nothing. When any
    // root's translation, at the press or moved, is not finite, the whole frame is skipped and the
    // last position stays, so the roots never move apart. Does nothing without roots, and once the
    // transaction ended (Esc: Transactions::cancelOpen). Fails with the transaction's errors; the
    // world then stays at the last position.
    Expected<void> preview(const Ray& ray);

    // Ends the drag: one undo entry for everything previewed. When no non-zero delta is applied (a
    // press and release without a move, nothing that can move, or a drag back to the start) it cancels
    // instead, so an Alt press leaves no copies and no "Translate" entry. Nothing after Esc.
    void commit();

    // Ends the drag and puts everything back, the duplicates removed.
    void cancel();

    // True until commit(), cancel() or Esc.
    [[nodiscard]] bool active() const noexcept { return m_scope.active(); }

    [[nodiscard]] GizmoHandle handle() const noexcept { return m_grab.handle; }

    // The world-space translation last applied; zero at first.
    [[nodiscard]] Vec3 delta() const noexcept { return m_delta; }

    // How many entities the drag moves (the selected roots).
    [[nodiscard]] std::size_t moved() const noexcept { return m_roots.size(); }

private:
    struct Root {
        Entity entity;
        // The local translation at the press.
        Vec3 translation{0.0f};
        // World-space directions to the parent's space: the inverse of the parent's world matrix's
        // 3x3 (identity for a root of the world).
        Mat3 fromWorld{1.0f};
    };

    GizmoDrag(EditorSession& session, TransactionScope scope, const GizmoGrab& grab, PropertyPath translation,
              std::vector<Root> roots);

    EditorSession* m_session;
    TransactionScope m_scope;
    GizmoGrab m_grab;
    // LocalTransform's "value.translation".
    PropertyPath m_translation;
    std::vector<Root> m_roots;
    Vec3 m_delta{0.0f};
};

} // namespace ith
