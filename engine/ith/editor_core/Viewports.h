#pragma once

// ith::Viewports: the state of the editor's viewports, without any UI (ADR-0042 rules 3 to 5, 20
// and 26).
//
//   ith::ViewportState* viewport = session.viewports().find("ith.viewport.1");
//   ith::updateCamera(viewport->camera, input, deltaSeconds);
//   session.execute("ith.viewport.rotate_tool");
//   const bool rotating = session.viewports().tool() == ith::ViewportTool::Rotate;
//
// One ViewportState per viewport panel instance, "ith.viewport.1" to "ith.viewport.4" (ADR-0040
// rules 2 and 3), whether its panel is open or not: the camera, the game-view flag and the image
// rectangle the panel last reported. The transform tool, the gizmo space and snapping are one
// editor-wide state next to them. Panels draw the store, the ith.viewport commands change it and
// headless tests drive it. None of it is level content: it never dirties the level or makes an
// undo entry.
//
// Cameras are remembered per level. saveLevel keeps the cameras for a level and writes the
// viewports file (<project>/Saved/Editor/Viewports.json) when what it holds changed; restoreLevel
// gives every viewport the camera saved for a level, or the default camera. The session calls them
// when a level closes and opens, and at exit (EditorSession::saveViewports). The file is read the
// first time either is called. A missing file (a fresh project) means default cameras, silently; an
// unreadable one means default cameras and one warning in the "EditorViewports" log category. A
// failed write is logged once until a write succeeds.
// The tool, the gizmo space, snapping and the game-view flags are session state and are not saved.
//
// The file is an ith.editor_viewports document, version 1; its data holds the cameras by level
// asset id, then by viewport instance id:
//
//   {"levels": {"<level id>": {"ith.viewport.1": {"position": {"x": 0, "y": 2, "z": 5}, "yaw": 0,
//                                                  "pitch": 0, "fovY": 1.5707964, "speedStep": 4,
//                                                  "pivot": {"x": 0, "y": 0, "z": 0}}, ...}}}
//
// Main thread only.

#include "ith/editor_core/EditorCamera.h"
#include "ith/foundation/Uuid.h"
#include "ith/math/Types.h"
#include "ith/serialization/Document.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace ith {

// The transform tool of every viewport (ADR-0042 rule 26).
enum class ViewportTool : std::uint8_t {
    Translate,
    Rotate,
    Scale,
};

// What the gizmo's axes follow (ADR-0042 rule 26).
enum class GizmoSpace : std::uint8_t {
    World, // the world axes
    Local, // the primary selected entity's rotation
};

// The viewport panel's id; its instances are "ith.viewport.1" to "ith.viewport.4".
inline constexpr std::string_view kViewportPanel = "ith.viewport";

// The viewport that commands taking an optional "viewport" argument use when it is left out.
inline constexpr std::string_view kDefaultViewport = "ith.viewport.1";

// How many viewport instances there are (ADR-0040 rule 3).
inline constexpr std::size_t kMaxViewports = 4;

// The near plane of every viewport's projection, in metres (ADR-0042 rule 5).
inline constexpr float kViewportNearPlane = 0.1f;

// The snapping step a session starts with, in metres (ADR-0042 rule 26).
inline constexpr float kDefaultSnapStep = 0.1f;

// The viewports file, relative to the project directory (ADR-0042 rule 4).
inline constexpr std::string_view kViewportsFile = "Saved/Editor/Viewports.json";

// The viewports file's format (ADR-0042 rule 4).
inline constexpr serialization::DocumentFormat kEditorViewportsFormat{
    .name = "ith.editor_viewports", .version = 1, .minimumVersion = 1};

// The largest viewports file read.
inline constexpr std::size_t kMaxViewportsFileSize = std::size_t{4} << 20;

// One viewport's state.
struct ViewportState {
    // Saved per level.
    EditorCamera camera;
    // Draws what a game would see: no grid, gizmo or selection tint (ADR-0042 rule 20).
    bool gameView = false;
    // The panel's image rectangle in window pixels, as the panel last reported it (ADR-0042 rule 8);
    // zero size until the panel has drawn.
    Vec2 imagePosition{0.0f};
    Vec2 imageSize{0.0f};

    // The image's width / height; 1 while the image has no area.
    [[nodiscard]] float aspect() const noexcept {
        return imageSize.x > 0.0f && imageSize.y > 0.0f ? imageSize.x / imageSize.y : 1.0f;
    }
};

class Viewports {
public:
    // `file` is the viewports file's path; empty: cameras are kept for the session only, and no
    // file is read or written. Every viewport starts with the default camera.
    explicit Viewports(std::string file = {});

    Viewports(const Viewports&) = delete;
    Viewports& operator=(const Viewports&) = delete;
    Viewports(Viewports&&) = delete;
    Viewports& operator=(Viewports&&) = delete;
    ~Viewports() = default;

    // The state of the viewport instance `instance` ("ith.viewport.1" to "ith.viewport.4"); null
    // for any other id.
    [[nodiscard]] ViewportState* find(std::string_view instance) noexcept;
    [[nodiscard]] const ViewportState* find(std::string_view instance) const noexcept;

    // The instance id of viewport `index`, 0 to kMaxViewports - 1: "ith.viewport.<index + 1>".
    // Precondition (asserted): index < kMaxViewports; otherwise the last one's.
    [[nodiscard]] static std::string instanceId(std::size_t index);

    // The transform tool; Translate at first.
    [[nodiscard]] ViewportTool tool() const noexcept { return m_tool; }

    void setTool(ViewportTool tool) noexcept { m_tool = tool; }

    // The gizmo space; World at first.
    [[nodiscard]] GizmoSpace gizmoSpace() const noexcept { return m_gizmoSpace; }

    void setGizmoSpace(GizmoSpace space) noexcept { m_gizmoSpace = space; }

    // Whether a gizmo drag snaps its translation, per axis, to snapStep(); on at first.
    [[nodiscard]] bool snapEnabled() const noexcept { return m_snapEnabled; }

    // The snapping step in metres; kDefaultSnapStep at first.
    [[nodiscard]] float snapStep() const noexcept { return m_snapStep; }

    // Turns snapping on or off and sets its step. Precondition (asserted): `step` is finite and
    // positive; otherwise nothing changes.
    void setSnap(bool enabled, float step) noexcept;

    // Keeps the cameras of every viewport as `level`'s, then writes the viewports file when what it
    // would hold differs from what it holds. A level without kept cameras whose cameras are all the
    // default gets no entry.
    void saveLevel(const Uuid& level);

    // Gives every viewport the camera kept for `level`, or the default camera when there is none.
    // The game-view flags and image rectangles stay.
    void restoreLevel(const Uuid& level);

private:
    using Cameras = std::array<EditorCamera, kMaxViewports>;
    using LevelCameras = std::map<Uuid, Cameras>;

    // Reads the file the first time it is needed.
    void load();
    void write();

    std::string m_file;
    std::array<ViewportState, kMaxViewports> m_states;
    ViewportTool m_tool = ViewportTool::Translate;
    GizmoSpace m_gizmoSpace = GizmoSpace::World;
    bool m_snapEnabled = true;
    float m_snapStep = kDefaultSnapStep;
    bool m_loaded = false;
    // The cameras kept per level, and what the file holds once read or written.
    LevelCameras m_levels;
    std::optional<LevelCameras> m_written;
    bool m_writeFailed = false;
};

} // namespace ith
