#include "ith/editor_ui/Theme.h"

#include "ith/foundation/Assert.h"

#include <imgui.h>

#include <array>
#include <cmath>
#include <cstdint>

namespace ith {
namespace {

// ImGui's style colours are sRGB-encoded channels in 0..1; the renderer decodes the vertex colours
// they become (ADR-0029 rule 11).
ImVec4 toImGui(ThemeColor color) {
    const std::array<float, 4> channels = srgbColor(color);
    return {channels[0], channels[1], channels[2], channels[3]};
}

ThemeColor withAlpha(ThemeColor color, std::uint8_t alpha) {
    color.alpha = alpha;
    return color;
}

void setColors(ImGuiStyle& style, const ThemeTokens& theme) {
    ImVec4* colors = style.Colors;
    const auto set = [colors](ImGuiCol slot, ThemeColor color) { colors[slot] = toImGui(color); };
    set(ImGuiCol_Text, theme.text);
    set(ImGuiCol_TextDisabled, theme.textDisabled);
    set(ImGuiCol_WindowBg, theme.panel);
    set(ImGuiCol_ChildBg, withAlpha(theme.panel, 0));
    set(ImGuiCol_PopupBg, theme.popup);
    set(ImGuiCol_Border, theme.border);
    set(ImGuiCol_BorderShadow, withAlpha(theme.border, 0));
    set(ImGuiCol_FrameBg, theme.control);
    set(ImGuiCol_FrameBgHovered, theme.controlHovered);
    set(ImGuiCol_FrameBgActive, theme.controlActive);
    set(ImGuiCol_TitleBg, theme.background);
    set(ImGuiCol_TitleBgActive, theme.panel);
    set(ImGuiCol_TitleBgCollapsed, theme.background);
    set(ImGuiCol_MenuBarBg, theme.panel);
    set(ImGuiCol_ScrollbarBg, withAlpha(theme.background, 0));
    set(ImGuiCol_ScrollbarGrab, theme.control);
    set(ImGuiCol_ScrollbarGrabHovered, theme.controlHovered);
    set(ImGuiCol_ScrollbarGrabActive, theme.controlActive);
    set(ImGuiCol_CheckMark, theme.accent);
    set(ImGuiCol_SliderGrab, theme.accent);
    set(ImGuiCol_SliderGrabActive, theme.accentHovered);
    set(ImGuiCol_Button, theme.control);
    set(ImGuiCol_ButtonHovered, theme.controlHovered);
    set(ImGuiCol_ButtonActive, theme.controlActive);
    set(ImGuiCol_Header, theme.control);
    set(ImGuiCol_HeaderHovered, theme.controlHovered);
    set(ImGuiCol_HeaderActive, theme.controlActive);
    set(ImGuiCol_Separator, theme.border);
    set(ImGuiCol_SeparatorHovered, theme.accentHovered);
    set(ImGuiCol_SeparatorActive, theme.accent);
    set(ImGuiCol_ResizeGrip, withAlpha(theme.border, 0));
    set(ImGuiCol_ResizeGripHovered, theme.accentHovered);
    set(ImGuiCol_ResizeGripActive, theme.accent);
    set(ImGuiCol_InputTextCursor, theme.text);
    set(ImGuiCol_Tab, theme.background);
    set(ImGuiCol_TabHovered, theme.controlHovered);
    set(ImGuiCol_TabSelected, theme.panel);
    set(ImGuiCol_TabSelectedOverline, theme.accent);
    set(ImGuiCol_TabDimmed, theme.background);
    set(ImGuiCol_TabDimmedSelected, theme.panel);
    set(ImGuiCol_TabDimmedSelectedOverline, withAlpha(theme.accent, 0));
    set(ImGuiCol_DockingPreview, withAlpha(theme.accent, 160));
    set(ImGuiCol_DockingEmptyBg, theme.background);
    set(ImGuiCol_TableHeaderBg, theme.control);
    set(ImGuiCol_TableBorderStrong, theme.border);
    set(ImGuiCol_TableBorderLight, theme.border);
    set(ImGuiCol_TableRowBg, withAlpha(theme.panel, 0));
    set(ImGuiCol_TableRowBgAlt, withAlpha(theme.text, 8));
    set(ImGuiCol_TextLink, theme.accent);
    set(ImGuiCol_TextSelectedBg, theme.selection);
    set(ImGuiCol_DragDropTarget, theme.accent);
    set(ImGuiCol_NavCursor, theme.accent);
    set(ImGuiCol_NavWindowingHighlight, withAlpha(theme.text, 180));
    set(ImGuiCol_NavWindowingDimBg, withAlpha(theme.background, 128));
    set(ImGuiCol_ModalWindowDimBg, withAlpha(theme.background, 160));
}

} // namespace

std::array<float, 4> srgbColor(ThemeColor color) {
    constexpr float kScale = 1.0f / 255.0f;
    return {static_cast<float>((color.rgb >> 16) & 0xFFu) * kScale,
            static_cast<float>((color.rgb >> 8) & 0xFFu) * kScale, static_cast<float>(color.rgb & 0xFFu) * kScale,
            static_cast<float>(color.alpha) * kScale};
}

std::array<float, 4> linearColor(ThemeColor color) {
    const auto channel = [](std::uint32_t value) {
        const float encoded = static_cast<float>(value & 0xFFu) / 255.0f;
        return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
    };
    return {channel(color.rgb >> 16), channel(color.rgb >> 8), channel(color.rgb),
            static_cast<float>(color.alpha) / 255.0f};
}

void applyTheme(const ThemeTokens& theme, float displayScale) {
    ITH_ASSERT_MSG(ImGui::GetCurrentContext() != nullptr, "applyTheme needs the editor UI's ImGui context");
    ITH_ASSERT_MSG(displayScale > 0.0f, "the display scale is positive");
    if (ImGui::GetCurrentContext() == nullptr || !(displayScale > 0.0f)) {
        return;
    }
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    setColors(style, theme);
    style.WindowPadding = {theme.windowPaddingX, theme.windowPaddingY};
    style.FramePadding = {theme.framePaddingX, theme.framePaddingY};
    style.ItemSpacing = {theme.itemSpacingX, theme.itemSpacingY};
    style.ItemInnerSpacing = {theme.itemSpacingY, theme.itemSpacingY};
    style.IndentSpacing = theme.indent;
    style.ScrollbarSize = theme.scrollbarSize;
    style.WindowRounding = theme.windowRounding;
    style.ChildRounding = theme.windowRounding;
    style.FrameRounding = theme.frameRounding;
    style.PopupRounding = theme.popupRounding;
    style.ScrollbarRounding = theme.frameRounding;
    style.GrabRounding = theme.frameRounding;
    style.TabRounding = theme.tabRounding;
    style.WindowBorderSize = theme.windowBorder;
    style.ChildBorderSize = theme.windowBorder;
    style.PopupBorderSize = theme.windowBorder;
    style.FrameBorderSize = theme.frameBorder;
    style.ScaleAllSizes(displayScale);
    style.FontSizeBase = theme.fontSize;
    style.FontScaleDpi = displayScale;
}

} // namespace ith
