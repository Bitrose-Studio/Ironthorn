// A lint fixture for the imgui_confined check (ADR-0029 rule 2): ImGui included outside the
// editor_ui module, and its internals outside DockLayout.cpp. It is never compiled.

#include <imgui.h>
#include <imgui_internal.h>
