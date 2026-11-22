// The editor shell (menus from the command registry with keymap chords, dock space, status bar),
// the icons checked against the pinned Lucide font, the UI shader source and the idle policy.

#include "ith/config/Config.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Keymap.h"
#include "ith/editor_ui/EditorShell.h"
#include "ith/editor_ui/EditorUiShader.h"
#include "ith/editor_ui/Icons.h"
#include "ith/editor_ui/IdlePolicy.h"
#include "ith/file_system/FileSystem.h"
#include "ith/reflection/TypeRegistry.h"

#include "EditorUiTestSupport.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <chrono>
#include <string>
#include <vector>

ITH_DECLARE_REFLECTION_MODULE(ecs)
ITH_DECLARE_REFLECTION_MODULE(editor_core)

using namespace ith;
using namespace ith::test::ui;

namespace {

const TypeRegistry& types() {
    static const TypeRegistry kTypes = [] {
        TypeRegistry result;
        generated::registerReflection_ecs(result);
        generated::registerReflection_editor_core(result);
        return result;
    }();
    return kTypes;
}

// The engine's keymap, as the editor loads it.
Keymap engineKeymap(const EditorSession& session) {
    const Config config({{.name = "engine", .directory = "engine/config"}});
    std::vector<Error> warnings;
    return Keymap::load(config, session.commands(), warnings);
}

} // namespace

TEST_SUITE("editor_ui.shell") {
    TEST_CASE("the File and Edit menus list their commands in menu order") {
        EditorSession session(types());
        const std::vector<std::string> file = menuCommands(session, "File");
        REQUIRE(file.size() >= 4);
        CHECK(file[0] == "ith.file.new_level");
        CHECK(file[1] == "ith.file.open_level");
        CHECK(file[2] == "ith.file.save");
        CHECK(file[3] == "ith.file.save_as");
        const std::vector<std::string> edit = menuCommands(session, "Edit");
        REQUIRE(edit.size() >= 2);
        CHECK(edit[0] == "ith.edit.undo");
        CHECK(edit[1] == "ith.edit.redo");
        for (const std::string& id : edit) {
            CHECK(session.commands().find(id)->category == "Edit");
        }
    }

    TEST_CASE("menus show the keymap's chords as keys") {
        EditorSession session(types());
        const Keymap keymap = engineKeymap(session);
        const std::vector<Chord> undo = keymap.chordsOf("ith.edit.undo");
        REQUIRE_FALSE(undo.empty());
        CHECK(chordLabel(undo.front()) == "Ctrl+Z");
        CHECK(chordLabel(keymap.chordsOf("ith.file.save_as").front()) == "Ctrl+Alt+S");
        CHECK(chordLabel(keymap.chordsOf("ith.edit.delete").front()) == "Delete");
    }

    TEST_CASE("the status bar names the level and marks unsaved changes") {
        EditorSession session(types());
        CHECK(documentTitle(session) == "Untitled");
        EditorShellState state;
        runMenuCommand(session, "ith.entity.create", state);
        CHECK(state.lastResult.hasValue());
        CHECK(state.status == "Create Entity");
        CHECK(documentTitle(session) == "Untitled *");
        runMenuCommand(session, "ith.edit.undo", state);
        CHECK(documentTitle(session) == "Untitled");
        // A command that can't run reports why.
        runMenuCommand(session, "ith.edit.redo", state);
        runMenuCommand(session, "ith.edit.redo", state);
        CHECK_FALSE(state.lastResult.hasValue());
        CHECK_FALSE(state.status.empty());
        CHECK(state.lastCommand == "ith.edit.redo");
    }

    TEST_CASE("the shell draws a main menu bar, the dock space and the status bar") {
        UiFixture fixture;
        EditorSession session(types());
        const Keymap keymap = engineKeymap(session);
        EditorShellState state;
        for (int i = 0; i < 3; ++i) {
            fixture.frame([&] { drawEditorShell(session, keymap, state); });
        }
        CHECK(fixture.ui->renderStats().draws > 0);
        // The menu bar takes the top of the window, so the work area starts below it.
        CHECK(ImGui::GetMainViewport()->WorkPos.y > 0.0f);

        // Open the File menu with a click on its title, then draw it open.
        fixture.events({{.type = EventType::MouseMoved, .x = 14.0f, .y = 8.0f}});
        fixture.frame([&] { drawEditorShell(session, keymap, state); });
        fixture.events({{.type = EventType::MouseButtonDown,
                         .mouseButton = MouseButton::Left,
                         .clicks = 1,
                         .x = 14.0f,
                         .y = 8.0f}});
        fixture.frame([&] { drawEditorShell(session, keymap, state); });
        fixture.events({{.type = EventType::MouseButtonUp, .mouseButton = MouseButton::Left, .x = 14.0f, .y = 8.0f}});
        fixture.frame([&] { drawEditorShell(session, keymap, state); });
        fixture.frame([&] { drawEditorShell(session, keymap, state); });
        CHECK(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
    }
}

TEST_SUITE("editor_ui.icons") {
    TEST_CASE("every icon name exists in the pinned font's codepoints.json with its codepoint") {
        const Expected<std::string> text = fs::readFile(ITH_LUCIDE_CODEPOINTS, std::size_t{4} << 20);
        REQUIRE(text.hasValue());
        for (const Icon& icon : icons::kAll) {
            CAPTURE(icon.name);
            const std::string key = "\"" + std::string(icon.name) + "\":";
            const std::size_t at = text->find(key);
            REQUIRE(at != std::string::npos);
            const std::size_t digits = text->find_first_of("0123456789", at + key.size());
            REQUIRE(digits != std::string::npos);
            CHECK(std::stoul(text->substr(digits, 8)) == icon.codepoint);
        }
    }

    TEST_CASE("icons are UTF-8 text, and built-in menu commands have one") {
        CHECK(iconText(icons::kSave) == "\xEE\x85\x8D");
        CHECK(iconText({.name = "a", .codepoint = U'a'}) == "a");
        CHECK(iconText({.name = "e", .codepoint = 0xE9}) == "\xC3\xA9");
        CHECK(iconText({.name = "smile", .codepoint = 0x1F600}) == "\xF0\x9F\x98\x80");
        REQUIRE(commandIcon("ith.edit.undo") != nullptr);
        CHECK(commandIcon("ith.edit.undo")->name == "undo-2");
        CHECK(commandIcon("ith.query.document") == nullptr);
    }
}

TEST_SUITE("editor_ui.shader") {
    TEST_CASE("the shader source carries both entry points and the constants of the renderer") {
        const std::string_view source = editorUiShaderSource();
        CHECK(source.find(kEditorUiVertexEntry) != std::string_view::npos);
        CHECK(source.find(kEditorUiPixelEntry) != std::string_view::npos);
        CHECK(source.find("srgbToLinear") != std::string_view::npos);
        CHECK(source.find("vertexOffset") != std::string_view::npos);
    }
}

TEST_SUITE("editor_ui.idle") {
    TEST_CASE("frames run at once after activity, then at the idle rate, lower without focus") {
        using std::chrono::milliseconds;
        IdlePolicy idle({.focused = 10, .unfocused = 2});
        // Start-up draws at once, then settles.
        for (std::uint32_t i = 0; i < 1 + IdlePolicy::kSettleFrames; ++i) {
            CHECK(idle.waitTimeout(true) == milliseconds(0));
            idle.frameFinished();
        }
        CHECK(idle.waitTimeout(true) == milliseconds(100));
        CHECK(idle.waitTimeout(false) == milliseconds(500));
        idle.frameFinished();
        CHECK(idle.pendingFrames() == 0);

        idle.requestFrame();
        CHECK(idle.pendingFrames() == 1 + IdlePolicy::kSettleFrames);
        CHECK(idle.waitTimeout(false) == milliseconds(0));
        idle.frameFinished();
        idle.requestFrame(); // activity again restarts the count
        CHECK(idle.pendingFrames() == 1 + IdlePolicy::kSettleFrames);

        idle.setRates({.focused = 0, .unfocused = -5});
        for (std::uint32_t i = 0; i < 1 + IdlePolicy::kSettleFrames; ++i) {
            idle.frameFinished();
        }
        CHECK(idle.waitTimeout(true) == milliseconds(1000));
        CHECK(idle.waitTimeout(false) == milliseconds(1000));
    }
}
