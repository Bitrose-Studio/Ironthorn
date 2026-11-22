#pragma once

// Dear ImGui's compile-time configuration for the engine (ADR-0029 rule 4): the
// IMGUI_USER_CONFIG header of the ith::imgui target, so every translation unit that includes
// imgui.h, ImGui's own included, sees the same settings. It never includes imgui.h itself.
//
// - IM_ASSERT is ITH_ASSERT: ImGui's checks report like the engine's and follow its build rules.
// - No obsolete functions, so upgrades show renamed APIs at once.
// - ImWchar is 32 bits wide, so text beyond the Basic Multilingual Plane (emoji, rare CJK) in
//   asset names keeps its code points. It changes the layout of ImGui's types, which is why it is
//   set here and nowhere else.
// - No Win32 or shell functions: the clipboard, the IME and the cursor go through ith::Platform
//   (platform code stays in the platform layer), and "open in shell" stays unset.
// - No default allocator: editor_ui installs the engine's allocation functions before it creates
//   the context (ImGui::SetAllocatorFunctions); an allocation without them asserts.

#include "ith/foundation/Assert.h"

#define IM_ASSERT(expression) ITH_ASSERT(expression)
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_USE_WCHAR32
#define IMGUI_DISABLE_WIN32_FUNCTIONS
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
#define IMGUI_DISABLE_DEFAULT_ALLOCATORS
