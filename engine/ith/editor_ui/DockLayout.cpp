#include "ith/editor_ui/DockLayout.h"

#include <imgui.h>

namespace ith {

void drawDockSpace(float bottomReserved) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 size{viewport->WorkSize.x, viewport->WorkSize.y - bottomReserved};
    if (size.x <= 0.0f || size.y <= 0.0f) {
        return;
    }
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowViewport(viewport->ID);
    // NOLINTBEGIN(bugprone-signed-bitwise): ImGui's flags are int enums
    constexpr ImGuiWindowFlags kHostFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoBackground;
    // NOLINTEND(bugprone-signed-bitwise)
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("Dock space###ith.dockspace_host", nullptr, kHostFlags);
    ImGui::PopStyleVar(3);
    ImGui::DockSpace(ImGui::GetID(kDockSpaceName), ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();
}

} // namespace ith
