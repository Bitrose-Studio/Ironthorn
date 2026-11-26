// The editor UI's platform backend and theme, headless: engine events injected through Input,
// routed by ImGui's want-capture flags (ADR-0029 rule 7), and the clipboard, IME and cursor
// requests the UI makes of its host, and the mouse left to the viewport consumer (ADR-0042 rules 9
// and 11): over a viewport image without a popup, during its drag, and relative mode while it flies.

#include "ith/editor_ui/EditorUi.h"

#include "ith/editor_core/EditorCamera.h"
#include "ith/editor_core/ViewportInput.h"
#include "ith/editor_core/Viewports.h"
#include "ith/editor_ui/Theme.h"

#include "AssertCapture.h"
#include "EditorUiTestSupport.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <array>
#include <cmath>
#include <string>
#include <vector>

using namespace ith;
using namespace ith::test::ui;

namespace {

Event mouseMoved(float x, float y) {
    return {.type = EventType::MouseMoved, .x = x, .y = y};
}

Event mouseButton(EventType type, float x, float y) {
    return {.type = type, .mouseButton = MouseButton::Left, .clicks = 1, .x = x, .y = y};
}

Event key(EventType type, Key physical, char32_t printed = 0) {
    return {.type = type, .key = physical, .layoutKey = printed};
}

// A window at a fixed place, covering (100, 100) to (500, 400).
void drawWindow() {
    ImGui::SetNextWindowPos({100.0f, 100.0f});
    ImGui::SetNextWindowSize({400.0f, 300.0f});
    ImGui::Begin("Test window");
    ImGui::TextUnformatted("Hello");
    ImGui::End();
}

// The same window with a popup open in it, at the mouse.
void drawWindowWithPopup() {
    ImGui::SetNextWindowPos({100.0f, 100.0f});
    ImGui::SetNextWindowSize({400.0f, 300.0f});
    ImGui::Begin("Test window");
    if (!ImGui::IsPopupOpen("Menu")) {
        ImGui::OpenPopup("Menu");
    }
    if (ImGui::BeginPopup("Menu")) {
        ImGui::TextUnformatted("Item");
        ImGui::EndPopup();
    }
    ImGui::End();
}

Event press(MouseButton button, float x, float y) {
    return {.type = EventType::MouseButtonDown, .mouseButton = button, .clicks = 1, .x = x, .y = y};
}

Event release(MouseButton button, float x, float y) {
    return {.type = EventType::MouseButtonUp, .mouseButton = button, .x = x, .y = y};
}

// A window with a text field that has keyboard focus.
void drawTextField(std::array<char, 64>& buffer, bool focus) {
    ImGui::SetNextWindowPos({100.0f, 100.0f});
    ImGui::SetNextWindowSize({400.0f, 300.0f});
    ImGui::Begin("Text window");
    if (focus) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputText("Name", buffer.data(), buffer.size());
    ImGui::End();
}

} // namespace

TEST_SUITE("editor_ui.platform") {
    TEST_CASE("a second EditorUi is refused while one exists") {
        UiFixture fixture;
        const test::AssertCapture capture;
        Input other;
        Expected<std::unique_ptr<EditorUi>> second = EditorUi::create({.device = &*fixture.device,
                                                                       .vertexShader = kShaderCode,
                                                                       .pixelShader = kShaderCode,
                                                                       .fontDirectory = fixture.fonts,
                                                                       .input = &other,
                                                                       .window = kWindow});
#ifdef ITH_ENABLE_ASSERTS
        CHECK(capture.count() == 1);
#endif
        REQUIRE_FALSE(second.hasValue());
        CHECK(second.error().code == "editor_ui.invalid_desc");
        // The first one still works.
        fixture.frame();
    }

    TEST_CASE("a missing font directory fails with editor_ui.font_failed and frees the context") {
        Expected<rhi::Device, rhi::Error> device = rhi::createNullDevice({});
        REQUIRE(device.hasValue());
        Input input;
        Expected<std::unique_ptr<EditorUi>> ui = EditorUi::create({.device = &*device,
                                                                   .vertexShader = kShaderCode,
                                                                   .pixelShader = kShaderCode,
                                                                   .fontDirectory = "no/such/fonts",
                                                                   .input = &input,
                                                                   .window = kWindow});
        REQUIRE_FALSE(ui.hasValue());
        CHECK(ui.error().code == "editor_ui.font_failed");
        CHECK(ImGui::GetCurrentContext() == nullptr);
        // Another one may be created afterwards.
        UiFixture fixture;
        fixture.frame();
    }

    TEST_CASE("ImGui writes no ini or log file, docks, and keeps floating panels in the main window") {
        UiFixture fixture;
        const ImGuiIO& io = ImGui::GetIO();
        CHECK(io.IniFilename == nullptr);
        CHECK(io.LogFilename == nullptr);
        CHECK((io.ConfigFlags & ImGuiConfigFlags_DockingEnable) != 0);
        CHECK((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0);
        CHECK((io.BackendFlags & ImGuiBackendFlags_RendererHasTextures) != 0);
        CHECK((io.BackendFlags & ImGuiBackendFlags_RendererHasVtxOffset) != 0);
    }

    TEST_CASE("a press over an ImGui window is consumed; over empty space it reaches the consumer below") {
        UiFixture fixture;
        // The hover comes in one frame; the flags it sets route the next frame's press.
        fixture.events({mouseMoved(200.0f, 200.0f)});
        fixture.frame(drawWindow);
        fixture.frame(drawWindow);
        CHECK(fixture.ui->wantsMouse());
        fixture.lower.events.clear();
        fixture.events({mouseButton(EventType::MouseButtonDown, 200.0f, 200.0f)});
        CHECK_FALSE(fixture.lower.saw(EventType::MouseButtonDown));
        fixture.frame(drawWindow);
        fixture.events({mouseButton(EventType::MouseButtonUp, 200.0f, 200.0f)});
        fixture.frame(drawWindow);

        fixture.events({mouseMoved(900.0f, 600.0f)});
        fixture.frame(drawWindow);
        fixture.frame(drawWindow);
        CHECK_FALSE(fixture.ui->wantsMouse());
        fixture.lower.events.clear();
        fixture.events({mouseButton(EventType::MouseButtonDown, 900.0f, 600.0f)});
        CHECK(fixture.lower.saw(EventType::MouseButtonDown));
    }

    TEST_CASE("event positions in window pixels become window coordinates") {
        UiFixture fixture;
        fixture.events({mouseMoved(400.0f, 300.0f)});
        // A display at 200%: the same pixels are half as far in window coordinates.
        fixture.ui->newFrame({.size = {kWidth / 2, kHeight / 2}, .pixelSize = {kWidth, kHeight}});
        fixture.ui->endFrame();
        fixture.render();
        fixture.events({mouseMoved(400.0f, 300.0f)});
        fixture.ui->newFrame({.size = {kWidth / 2, kHeight / 2}, .pixelSize = {kWidth, kHeight}});
        CHECK(ImGui::GetIO().MousePos.x == doctest::Approx(200.0f));
        CHECK(ImGui::GetIO().MousePos.y == doctest::Approx(150.0f));
        CHECK(ImGui::GetIO().DisplayFramebufferScale.x == doctest::Approx(2.0f));
        fixture.ui->endFrame();
        fixture.render();
    }

    TEST_CASE("keys reach the keymap below unless a text field is active, which then types them") {
        UiFixture fixture;
        std::array<char, 64> buffer{};
        fixture.frame([&] { drawTextField(buffer, false); });
        fixture.lower.events.clear();
        fixture.events({key(EventType::KeyDown, Key::W, U'w'), key(EventType::KeyUp, Key::W, U'w')});
        CHECK(fixture.lower.saw(EventType::KeyDown));
        CHECK_FALSE(fixture.input.textInputWanted(kWindow));

        // Focus the field: the next frame's routing sees ImGui wanting text.
        fixture.frame([&] { drawTextField(buffer, true); });
        fixture.frame([&] { drawTextField(buffer, false); });
        CHECK(fixture.ui->wantsTextInput());
        CHECK(fixture.input.focusedConsumer(kWindow) == fixture.ui->consumer());
        CHECK(fixture.input.textInputWanted(kWindow));
        fixture.lower.events.clear();
        fixture.events({key(EventType::KeyDown, Key::W, U'w'), Event{.type = EventType::TextInput, .text = "W"},
                        key(EventType::KeyUp, Key::W, U'w')});
        CHECK_FALSE(fixture.lower.saw(EventType::KeyDown));
        CHECK_FALSE(fixture.lower.saw(EventType::TextInput));
        fixture.frame([&] { drawTextField(buffer, false); });
        CHECK(std::string(buffer.data()) == "W");

        // Escape leaves the field; the UI gives the text focus back.
        fixture.events({key(EventType::KeyDown, Key::Escape), key(EventType::KeyUp, Key::Escape)});
        fixture.frame([&] { drawTextField(buffer, false); });
        fixture.frame([&] { drawTextField(buffer, false); });
        CHECK_FALSE(fixture.ui->wantsTextInput());
        CHECK_FALSE(fixture.input.textInputWanted(kWindow));
    }

    TEST_CASE("letter shortcuts in a text field follow the layout") {
        UiFixture fixture;
        // The physical Q key prints 'a' on AZERTY: ImGui sees A.
        fixture.events(
            {{.type = EventType::KeyDown, .key = Key::Q, .layoutKey = U'a', .modifiers = KeyModifiers::Control}});
        fixture.frame();
        CHECK(ImGui::IsKeyDown(ImGuiKey_A));
        CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_Q));
        CHECK(ImGui::GetIO().KeyCtrl);
        fixture.events({{.type = EventType::KeyUp, .key = Key::Q, .layoutKey = U'a'}});
        fixture.frame();
        CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_A));
    }

    TEST_CASE("losing focus releases what ImGui holds") {
        UiFixture fixture;
        fixture.events({key(EventType::KeyDown, Key::Enter)});
        fixture.frame();
        CHECK(ImGui::IsKeyDown(ImGuiKey_Enter));
        fixture.frame([] {}, false);
        CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_Enter));
    }

    TEST_CASE("the clipboard goes through the host") {
        UiFixture fixture;
        fixture.host.clipboard = "from the platform";
        fixture.frame([&] {
            CHECK(std::string(ImGui::GetClipboardText()) == "from the platform");
            ImGui::SetClipboardText("from ImGui");
        });
        CHECK(fixture.host.clipboardReads == 1);
        CHECK(fixture.host.clipboard == "from ImGui");
    }

    TEST_CASE("an active text field places the IME area in window pixels") {
        UiFixture fixture;
        std::array<char, 64> buffer{};
        fixture.frame([&] { drawTextField(buffer, true); });
        fixture.frame([&] { drawTextField(buffer, false); });
        REQUIRE(fixture.host.textInputArea.has_value());
        CHECK(fixture.host.textInputArea->x >= 100);
        CHECK(fixture.host.textInputArea->y >= 100);
        CHECK(fixture.host.textInputArea->height > 0);
    }

    TEST_CASE("the cursor the last frame asked for goes to the host, and a drawn cursor hides it") {
        UiFixture fixture;
        fixture.frame([] { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); });
        fixture.frame();
        CHECK(fixture.host.cursor == CursorShape::Hand);
        fixture.frame([] { ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput); });
        fixture.frame([] { ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput); });
        CHECK(fixture.host.cursor == CursorShape::TextInput);
        // The same shape frame after frame is handed over once.
        const int changes = fixture.host.cursorChanges;
        fixture.frame([] { ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput); });
        fixture.frame([] { ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput); });
        CHECK(fixture.host.cursorChanges == changes);
        fixture.frame([] { ImGui::SetMouseCursor(ImGuiMouseCursor_None); });
        fixture.frame();
        CHECK_FALSE(fixture.host.cursor.has_value());
    }

    TEST_CASE("while the mouse is captured, ImGui is told the mouse is gone and the window is relative") {
        UiFixture fixture;
        fixture.events({mouseMoved(200.0f, 200.0f)});
        fixture.frame(drawWindow);
        CHECK(ImGui::IsMousePosValid());
        CHECK(fixture.host.relativeMouseModeChanges == 0);
        REQUIRE(fixture.input.captureMouse(kWindow));
        fixture.frame(drawWindow);
        CHECK_FALSE(ImGui::IsMousePosValid());
        CHECK(fixture.host.relativeMouseMode);
        fixture.input.releaseMouse();
        fixture.frame(drawWindow);
        CHECK_FALSE(fixture.host.relativeMouseMode);
        CHECK(fixture.host.relativeMouseModeChanges == 2);
    }

    TEST_CASE("over a viewport image the mouse goes on to the viewport consumer, and a held drag keeps it") {
        Viewports viewports;
        // Inside the ImGui window, which wants the mouse over all of it.
        viewports.at(0).imagePosition = {150.0f, 150.0f};
        viewports.at(0).imageSize = {200.0f, 200.0f};
        viewports.at(0).imageHovered = true;
        ViewportConsumer viewportInput(viewports);
        UiFixture fixture({.viewports = &viewportInput}, &viewportInput);
        fixture.events({mouseMoved(200.0f, 200.0f)});
        fixture.frame(drawWindow);
        fixture.frame(drawWindow);
        REQUIRE(fixture.ui->wantsMouse());

        // RMB on the image flies: the viewport consumer takes the press, and the window turns relative.
        fixture.events({press(MouseButton::Right, 200.0f, 200.0f)});
        CHECK(viewportInput.flying());
        fixture.frame(drawWindow);
        CHECK(fixture.host.relativeMouseMode);
        CHECK(fixture.host.relativeMouseModeChanges == 1);

        // The drag leaves the image for the rest of the ImGui window: the viewport still gets the motion.
        const EditorCamera before = viewports.at(0).camera;
        fixture.events(
            {Event{.type = EventType::MouseMoved, .x = 450.0f, .y = 350.0f, .deltaX = 250.0f, .deltaY = 0.0f}});
        CHECK(viewports.at(0).camera != before);
        fixture.events({release(MouseButton::Right, 450.0f, 350.0f)});
        CHECK_FALSE(viewportInput.holdsDrag());
        fixture.frame(drawWindow);
        CHECK_FALSE(fixture.host.relativeMouseMode);
        CHECK(fixture.host.relativeMouseModeChanges == 2);
        // The cursor went back to the press, before relative mode let it show again.
        CHECK(fixture.host.mouseCalls == std::vector<std::string>{"relative on", "warp", "relative off"});
        REQUIRE(fixture.host.warpedTo.has_value());
        CHECK(fixture.host.warpedTo->first == 200.0f);
        CHECK(fixture.host.warpedTo->second == 200.0f);

        // Off the image, ImGui keeps its window's presses.
        fixture.frame(drawWindow);
        fixture.events({press(MouseButton::Middle, 450.0f, 350.0f)});
        CHECK_FALSE(viewportInput.holdsDrag());
        fixture.events({release(MouseButton::Middle, 450.0f, 350.0f)});
        fixture.frame(drawWindow);

        // While a popup is open ImGui keeps the presses on the image too.
        fixture.events({mouseMoved(200.0f, 200.0f)});
        fixture.frame(drawWindowWithPopup);
        fixture.frame(drawWindowWithPopup);
        REQUIRE(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
        fixture.events({press(MouseButton::Right, 300.0f, 300.0f)});
        CHECK_FALSE(viewportInput.holdsDrag());
        fixture.events({release(MouseButton::Right, 300.0f, 300.0f)});
    }

    TEST_CASE("during a held viewport drag ImGui sees no presses, motion or wheel") {
        Viewports viewports;
        viewports.at(0).imagePosition = {150.0f, 150.0f};
        viewports.at(0).imageSize = {200.0f, 200.0f};
        viewports.at(0).imageHovered = true;
        ViewportConsumer viewportInput(viewports);
        UiFixture fixture({.viewports = &viewportInput}, &viewportInput);
        // A second window with a button and more lines than fit, so the wheel scrolls it.
        int clicks = 0;
        float scrollY = 0.0f;
        UiRect button;
        const auto draw = [&] {
            drawWindow();
            ImGui::SetNextWindowPos({600.0f, 100.0f});
            ImGui::SetNextWindowSize({300.0f, 200.0f});
            ImGui::Begin("Buttons");
            if (ImGui::Button("Press")) {
                ++clicks;
            }
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            button = {.x = min.x, .y = min.y, .width = max.x - min.x, .height = max.y - min.y};
            for (int line = 0; line < 50; ++line) {
                ImGui::Text("Line %d", line);
            }
            scrollY = ImGui::GetScrollY();
            ImGui::End();
        };
        fixture.frame(draw);
        const float x = button.centerX();
        const float y = button.centerY();
        const auto clickAndScroll = [&] {
            fixture.events({mouseMoved(x, y)});
            fixture.frame(draw);
            fixture.events({press(MouseButton::Left, x, y)});
            fixture.frame(draw);
            fixture.events({release(MouseButton::Left, x, y)});
            fixture.frame(draw);
            fixture.events({Event{.type = EventType::MouseWheel, .x = x, .y = y, .deltaY = -3.0f}});
            fixture.frame(draw);
            fixture.frame(draw);
        };

        // Without a drag the button clicks and the wheel scrolls.
        clickAndScroll();
        CHECK(clicks == 1);
        CHECK(scrollY > 0.0f);
        fixture.events({Event{.type = EventType::MouseWheel, .x = x, .y = y, .deltaY = 30.0f}});
        fixture.frame(draw);
        fixture.frame(draw);
        REQUIRE(scrollY == 0.0f);

        // During a fly started on the image, neither reaches ImGui; the release still does.
        fixture.events({mouseMoved(200.0f, 200.0f)});
        fixture.frame(draw);
        fixture.frame(draw);
        fixture.events({press(MouseButton::Right, 200.0f, 200.0f)});
        REQUIRE(viewportInput.flying());
        clickAndScroll();
        CHECK(clicks == 1);
        CHECK(scrollY == 0.0f);
        CHECK_FALSE(ImGui::IsMousePosValid()); // ImGui was told the mouse is gone
        fixture.events({release(MouseButton::Right, x, y)});
        CHECK_FALSE(viewportInput.holdsDrag());
        fixture.frame(draw);
        CHECK_FALSE(ImGui::IsMouseDown(ImGuiMouseButton_Right));
    }

    TEST_CASE("losing focus during fly ends relative mode on the next frame") {
        Viewports viewports;
        viewports.at(0).imagePosition = {150.0f, 150.0f};
        viewports.at(0).imageSize = {200.0f, 200.0f};
        viewports.at(0).imageHovered = true;
        ViewportConsumer viewportInput(viewports);
        UiFixture fixture({.viewports = &viewportInput}, &viewportInput);
        fixture.events({press(MouseButton::Right, 200.0f, 200.0f)});
        REQUIRE(viewportInput.flying());
        fixture.frame();
        REQUIRE(fixture.host.relativeMouseMode);
        fixture.events({Event{.type = EventType::WindowFocusLost}});
        CHECK_FALSE(viewportInput.holdsDrag());
        fixture.frame([] {}, false);
        CHECK_FALSE(fixture.host.relativeMouseMode);
    }
}

TEST_SUITE("editor_ui.theme") {
    TEST_CASE("the theme sets ImGui's colours from sRGB tokens and scales its sizes by the display scale") {
        UiFixture fixture;
        const ThemeTokens theme = defaultDarkTheme();
        applyTheme(theme, 1.5f);
        const ImGuiStyle& style = ImGui::GetStyle();
        CHECK(style.WindowPadding.x == doctest::Approx(theme.windowPaddingX * 1.5f));
        // ImGui rounds scaled sizes down to whole pixels.
        CHECK(style.FramePadding.y == doctest::Approx(std::trunc(theme.framePaddingY * 1.5f)));
        CHECK(style.FrameRounding == doctest::Approx(std::trunc(theme.frameRounding * 1.5f)));
        CHECK(style.FontSizeBase == doctest::Approx(theme.fontSize));
        CHECK(style.FontScaleDpi == doctest::Approx(1.5f));
        const ImVec4 panel = style.Colors[ImGuiCol_WindowBg];
        CHECK(panel.x == doctest::Approx(static_cast<float>((theme.panel.rgb >> 16) & 0xFF) / 255.0f));
        CHECK(panel.y == doctest::Approx(static_cast<float>((theme.panel.rgb >> 8) & 0xFF) / 255.0f));
        CHECK(panel.z == doctest::Approx(static_cast<float>(theme.panel.rgb & 0xFF) / 255.0f));
        CHECK(panel.w == doctest::Approx(1.0f));
        CHECK(style.Colors[ImGuiCol_TextSelectedBg].w ==
              doctest::Approx(static_cast<float>(theme.selection.alpha) / 255.0f));

        // A display scale change through the UI rebuilds the sizes from the tokens, not from the
        // scaled ones.
        fixture.ui->setDisplayScale(2.0f);
        CHECK(ImGui::GetStyle().WindowPadding.x == doctest::Approx(theme.windowPaddingX * 2.0f));
        CHECK(fixture.ui->displayScale() == doctest::Approx(2.0f));
        fixture.ui->setDisplayScale(1.0f);
        CHECK(ImGui::GetStyle().WindowPadding.x == doctest::Approx(theme.windowPaddingX));
        fixture.frame(drawWindow);
    }

    TEST_CASE("the fonts load, with the icon font merged into the text fonts") {
        UiFixture fixture;
        fixture.frame([&] {
            for (const UiFont font : {UiFont::Text, UiFont::Bold, UiFont::Mono}) {
                fixture.ui->pushFont(font);
                CHECK(ImGui::GetFont() != nullptr);
                ImGui::TextUnformatted("Ironthorn \xC5\x91"); // with Hungarian Å‘
                fixture.ui->popFont();
            }
            // U+E14D, Lucide's "save", is in the merged text font.
            CHECK(ImGui::GetFont()->IsGlyphInFont(0xE14D));
        });
    }
}
