#pragma once

// The dock space that panels dock into (ADR-0029 rule 15). DockLayout.cpp is the one file that
// may use Dear ImGui's docking internals (imgui_internal.h, DockBuilder); the layout documents and
// their commands come with the docking step, and until then the dock space starts empty. No ImGui
// type appears here.

namespace ith {

// The stable ImGui id name of the editor's dock space.
inline constexpr const char* kDockSpaceName = "ith.dockspace";

// Draws the dock space over the main window's work area (below the main menu bar), leaving
// `bottomReserved` pixels free at the bottom for the status bar. Inside a UI frame.
void drawDockSpace(float bottomReserved);

} // namespace ith
