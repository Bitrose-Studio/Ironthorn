#pragma once

// ith::DockLayout: the bridge between the editor's layout model and Dear ImGui's docking
// (ADR-0029 rule 15, ADR-0040 rules 10 to 13). DockLayout.cpp is the one file that may use ImGui's
// docking internals (imgui_internal.h, DockBuilder); no ImGui type appears here.
//
//   ith::DockLayout dock(session);           // after the panels are registered
//   session.layouts().restore();
//   while (running) {
//       ui.newFrame(...);
//       dock.draw(statusBarHeight, toolbarHeight); // the dock space and every open panel
//       ui.endFrame();
//   }
//   dock.readBack();                         // at exit: what the user arranged, into the session file
//
// - Applying: when the layout session's generation changes, the dock tree is rebuilt from the
//   current layout with DockBuilder: splits (each split's direction chosen so the central stack
//   stays ImGui's central node), tabs in order, floating groups and the active tabs.
// - Reading back: the dock tree becomes a layout handed to LayoutSession::observe() when ImGui
//   marks its settings dirty (a drag, a dock, a resize; ImGui debounces it), on the frame after a
//   rebuild, before every layout command (the session's read-back hook) and on readBack().
// - Panels: every open panel instance gets a window "<title>###<instance id>" whose content is its
//   PanelDesc::draw; its tab's close button runs "ith.window.close_panel", and Open Panel's focus
//   requests focus it.
// - A display-scale change (displayScaleChanged) reads back and rebuilds, so proportions survive
//   the theme's new sizes.
//
// One DockLayout per session, on the main thread, while the EditorUi exists.

#include <cstdint>
#include <string>
#include <vector>

namespace ith {

class EditorSession;

// The stable ImGui id name of the editor's dock space.
inline constexpr const char* kDockSpaceName = "ith.dockspace";

class DockLayout {
public:
    // Installs the read-back hook on the session's layouts; `session` must outlive this.
    explicit DockLayout(EditorSession& session);
    // Removes the hook.
    ~DockLayout();

    DockLayout(const DockLayout&) = delete;
    DockLayout& operator=(const DockLayout&) = delete;
    DockLayout(DockLayout&&) = delete;
    DockLayout& operator=(DockLayout&&) = delete;

    // Draws the dock space over the main window's work area (below the main menu bar), leaving
    // `bottomReserved` window units free at the bottom for the status bar and `topReserved` at the
    // top for the main toolbar, and every open panel. Inside a UI frame.
    void draw(float bottomReserved, float topReserved = 0.0f);

    // Reads ImGui's dock tree into the layout session now. Does nothing before the first draw.
    void readBack();

    // The window's display scale changed: the next draw reads back and rebuilds.
    void displayScaleChanged() noexcept { m_rescale = true; }

    // How many times the dock tree was rebuilt, for tests.
    [[nodiscard]] std::uint64_t rebuilds() const noexcept { return m_rebuilds; }

private:
    void build();
    void selectActiveTabs();

    EditorSession* m_session;
    std::uint32_t m_dockSpace = 0;
    // The dock space's rectangle, for floating rectangles as fractions of it.
    float m_x = 0.0f;
    float m_y = 0.0f;
    float m_width = 0.0f;
    float m_height = 0.0f;
    std::uint64_t m_applied = 0;
    bool m_built = false;
    bool m_rescale = false;
    // Frames until a rebuilt tree has settled: ImGui docks the windows on the first, the active tabs
    // are selected on the second, and the tree is read back after it.
    int m_settleFrames = 0;
    // The window names of the active tabs of the last build.
    std::vector<std::string> m_actives;
    std::uint64_t m_rebuilds = 0;
};

} // namespace ith
