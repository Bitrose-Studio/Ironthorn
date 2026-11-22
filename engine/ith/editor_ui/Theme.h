#pragma once

// The editor's look as design tokens (ADR-0029 rules 8, 11 and 13): colours, spacing, radii and
// type sizes, which applyTheme() maps onto Dear ImGui's style. Layout, panel names and shortcuts
// are not the theme's business; ADR-0008 decides them.
//
// Colours are sRGB hex values (0xRRGGBB), as designers give them; the renderer decodes them to
// linear before blending. Sizes are in display-independent units: applyTheme() multiplies them by
// the window's display scale, so a 150% display gets 1.5 times larger spacing and text without
// rebuilding the font atlas.
//
// defaultDarkTheme() is a neutral dark theme until the editor theme's values are set.

#include <cstdint>

namespace ith {

// An sRGB colour, 0xRRGGBB, with an alpha of 0 to 255.
struct ThemeColor {
    std::uint32_t rgb = 0;
    std::uint8_t alpha = 255;

    friend bool operator==(const ThemeColor&, const ThemeColor&) = default;
};

struct ThemeTokens {
    // Surfaces, darkest first: the window behind every panel, panels, raised controls (frames,
    // buttons, headers), and popups and menus.
    ThemeColor background{0x1A1A1C};
    ThemeColor panel{0x232326};
    ThemeColor control{0x2E2E32};
    ThemeColor controlHovered{0x3A3A3F};
    ThemeColor controlActive{0x46464C};
    ThemeColor popup{0x26262A};
    ThemeColor border{0x3A3A3F};
    // Text, and text of disabled controls.
    ThemeColor text{0xE2E2E5};
    ThemeColor textDisabled{0x808086};
    // The accent: the active tab, checks, sliders, selection and focus.
    ThemeColor accent{0x4C8DDA};
    ThemeColor accentHovered{0x5E9BE2};
    ThemeColor selection{0x4C8DDA, 96};
    // Status colours, for the Output Log and the status bar.
    ThemeColor warning{0xE0B040};
    ThemeColor error{0xE05050};

    // The base type size, and the monospace size, in units.
    float fontSize = 14.0f;
    float monoFontSize = 13.0f;
    // Padding inside windows, frames (controls) and between items, horizontal then vertical.
    float windowPaddingX = 8.0f;
    float windowPaddingY = 8.0f;
    float framePaddingX = 6.0f;
    float framePaddingY = 3.0f;
    float itemSpacingX = 8.0f;
    float itemSpacingY = 4.0f;
    float indent = 16.0f;
    float scrollbarSize = 12.0f;
    // Corner radii.
    float windowRounding = 0.0f;
    float frameRounding = 3.0f;
    float popupRounding = 3.0f;
    float tabRounding = 3.0f;
    // Border widths of windows and frames; 0 draws none.
    float windowBorder = 1.0f;
    float frameBorder = 0.0f;

    friend bool operator==(const ThemeTokens&, const ThemeTokens&) = default;
};

// The neutral dark theme the editor uses by default.
[[nodiscard]] inline ThemeTokens defaultDarkTheme() {
    return {};
}

// Applies `theme` to the current ImGui context's style, its sizes times `displayScale`: resets
// the style, sets its colour table from the tokens, scales every size, and sets the font size and
// the DPI font scale. Call it at start-up and on WindowDisplayScaleChanged.
// Preconditions (asserted): an ImGui context exists (EditorUi owns it); `displayScale` is positive.
void applyTheme(const ThemeTokens& theme, float displayScale);

} // namespace ith
