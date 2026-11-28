// The keymap (ADR-0037 rule 4, ADR-0029 rules 7 and 16, ADR-0049 rule 3): chords, layers where the
// highest listing of a command wins and [] or null unbinds, conflicts within a context, the Play In
// Editor and Content contexts, the engine's Keymap.json, and the consumer: letters by layout,
// repeats, Escape during an interaction, and refused commands.

#include "ith/editor_core/Keymap.h"

#include "ith/config/Config.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Transactions.h"
#include "ith/platform_events/Event.h"

#include "EditorCoreComponents.h"
#include "EditorTestSupport.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <ostream> // doctest prints std::string operands through an ostream
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ith::Chord;
using ith::EditorSession;
using ith::Key;
using ith::Keymap;
using ith::KeyModifiers;
using ith::test::editor::json;
using ith::test::editor::run;
using ith::test::editor::types;

Keymap keymapOf(const EditorSession& session, const std::vector<std::string_view>& layers,
                std::vector<ith::Error>& warnings) {
    std::vector<ith::serialization::Value> values;
    for (const std::string_view layer : layers) {
        values.push_back(json(layer));
    }
    std::vector<const ith::serialization::Value*> pointers;
    for (const ith::serialization::Value& value : values) {
        pointers.push_back(&value);
    }
    return Keymap::fromLayers(pointers, session.commands(), warnings);
}

Chord chord(std::string_view text) {
    const auto parsed = ith::parseChord(text);
    REQUIRE_MESSAGE(parsed.hasValue(), (parsed.hasValue() ? std::string() : parsed.error().message));
    return *parsed;
}

bool hasWarning(const std::vector<ith::Error>& warnings, std::string_view code) {
    return std::ranges::any_of(warnings, [&](const ith::Error& warning) { return warning.code == code; });
}

ith::Event keyDown(Key key, char32_t layoutKey, KeyModifiers modifiers = KeyModifiers::None, bool repeat = false) {
    return ith::Event{
        .type = ith::EventType::KeyDown, .key = key, .layoutKey = layoutKey, .modifiers = modifiers, .repeat = repeat};
}

} // namespace

TEST_SUITE("editor_core.keymap") {
    TEST_CASE("chords parse and print; bad ones are refused") {
        const Chord undo = chord("Ctrl+Keyboard.Z");
        CHECK(undo.key == Key::Z);
        CHECK(undo.modifiers == KeyModifiers::Control);
        CHECK(ith::toString(chord("Alt+Shift+Ctrl+Keyboard.S")) == "Ctrl+Shift+Alt+Keyboard.S");
        CHECK(ith::toString(chord("Keyboard.F2")) == "Keyboard.F2");
        for (const std::string_view bad :
             {"Ctrl+Ctrl+Keyboard.Z", "Super+Keyboard.Z", "Keyboard.Nothing", "Mouse.Left", "", "Ctrl+"}) {
            CAPTURE(bad);
            const auto parsed = ith::parseChord(bad);
            REQUIRE_FALSE(parsed.hasValue());
            CHECK(parsed.error().code == "editor.keymap.invalid_chord");
        }
    }

    TEST_CASE("layers: the highest listing of a command wins; [] and null unbind; conflicts and unknown ids warn") {
        EditorSession session(types());
        std::vector<ith::Error> warnings;
        const Keymap keymap =
            keymapOf(session,
                     {R"({"ith.edit.undo": ["Ctrl+Keyboard.Z"], "ith.edit.redo": ["Ctrl+Keyboard.Y"],
                                            "ith.edit.rename": ["Keyboard.F2"], "ith.edit.delete": ["Keyboard.Delete"]})",
                      R"({"ith.edit.redo": ["Ctrl+Shift+Keyboard.Z", "Ctrl+Keyboard.Y"], "ith.edit.rename": [],
                                            "ith.edit.delete": null, "ith.edit.duplicate": ["Ctrl+Keyboard.Z", "Bad.Chord"],
                                            "game.unknown": ["Keyboard.F9"]})"},
                     warnings);
        CHECK(keymap.chordsOf("ith.edit.redo") ==
              std::vector<Chord>{chord("Ctrl+Shift+Keyboard.Z"), chord("Ctrl+Keyboard.Y")});
        CHECK(keymap.chordsOf("ith.edit.rename").empty());
        CHECK(keymap.chordsOf("ith.edit.delete").empty());
        // duplicate (higher layer) takes Ctrl+Z from undo (lower layer).
        CHECK(*keymap.commandFor(chord("Ctrl+Keyboard.Z")) == "ith.edit.duplicate");
        CHECK(keymap.chordsOf("ith.edit.undo").empty());
        CHECK(hasWarning(warnings, "editor.keymap.conflict"));
        CHECK(hasWarning(warnings, "editor.keymap.invalid_chord"));
        CHECK(hasWarning(warnings, "editor.keymap.unknown_command"));
        CHECK(*keymap.commandFor(chord("Keyboard.F9")) == "game.unknown"); // kept for when it is registered

        std::vector<ith::Error> sameLayer;
        const Keymap tie =
            keymapOf(session, {R"({"ith.edit.select_none": ["Keyboard.F3"], "ith.edit.select_all": ["Keyboard.F3"]})"},
                     sameLayer);
        CHECK(*tie.commandFor(chord("Keyboard.F3")) == "ith.edit.select_all"); // the lower id
        CHECK(hasWarning(sameLayer, "editor.keymap.conflict"));
    }

    TEST_CASE("the Play In Editor keys have their own context, so Escape is bound twice") {
        EditorSession session(types());
        std::vector<ith::Error> warnings;
        const Keymap keymap = keymapOf(
            session, {R"({"ith.edit.select_none": ["Keyboard.Escape"], "ith.play.stop": ["Keyboard.Escape"]})"},
            warnings);
        CHECK(*keymap.commandFor(chord("Keyboard.Escape")) == "ith.edit.select_none");
        CHECK(*keymap.commandFor(chord("Keyboard.Escape"), ith::KeymapContext::PlayInEditor) == "ith.play.stop");
        CHECK_FALSE(hasWarning(warnings, "editor.keymap.conflict"));
    }

    TEST_CASE("the Content context: content commands bind beside the Editor's, conflicts are per context") {
        EditorSession session(types());
        std::vector<ith::Error> warnings;
        const Keymap keymap = keymapOf(session, {R"({"ith.edit.select_all": ["Ctrl+Keyboard.A"],
                                            "ith.content.select_all": ["Ctrl+Keyboard.A"],
                                            "ith.content.toggle_drawer": ["Ctrl+Keyboard.Space"]})"},
                                       warnings);
        CHECK(*keymap.commandFor(chord("Ctrl+Keyboard.A")) == "ith.edit.select_all");
        CHECK(*keymap.commandFor(chord("Ctrl+Keyboard.A"), ith::KeymapContext::Content) == "ith.content.select_all");
        // The drawer's toggle is an Editor binding: it works with the focus anywhere.
        CHECK(*keymap.commandFor(chord("Ctrl+Keyboard.Space")) == "ith.content.toggle_drawer");
        CHECK(keymap.commandFor(chord("Ctrl+Keyboard.Space"), ith::KeymapContext::Content) == nullptr);
        CHECK_FALSE(hasWarning(warnings, "editor.keymap.conflict"));

        std::vector<ith::Error> clash;
        const Keymap twice = keymapOf(
            session, {R"({"ith.content.select_all": ["Keyboard.F3"], "ith.content.dismiss": ["Keyboard.F3"]})"}, clash);
        CHECK(*twice.commandFor(chord("Keyboard.F3"), ith::KeymapContext::Content) == "ith.content.dismiss");
        CHECK(hasWarning(clash, "editor.keymap.conflict"));
    }

    TEST_CASE("the engine's Keymap.json loads cleanly and names only registered commands (requirement 45)") {
        EditorSession session(types());
        // The engine layer as the build copies it: engine/config (the tests run from the source directory).
        const ith::Config config({{.name = "engine", .directory = "engine/config"}});
        std::vector<ith::Error> warnings;
        const Keymap keymap = Keymap::load(config, session.commands(), warnings);
        for (const ith::Error& warning : warnings) {
            CAPTURE(warning.message);
            FAIL_CHECK("a warning"); // every bound command is registered, Play In Editor too (ADR-0053)
        }
        CHECK(*keymap.commandFor(chord("Ctrl+Keyboard.Z")) == "ith.edit.undo");
        CHECK(*keymap.commandFor(chord("Ctrl+Alt+Keyboard.S")) == "ith.file.save_as");
        CHECK(*keymap.commandFor(chord("Keyboard.Escape")) == "ith.edit.select_none");
        CHECK(*keymap.commandFor(chord("Keyboard.Escape"), ith::KeymapContext::PlayInEditor) == "ith.play.stop");
        CHECK(*keymap.commandFor(chord("Alt+Keyboard.P")) == "ith.play.play");
        CHECK(*keymap.commandFor(chord("Alt+Keyboard.S")) == "ith.play.simulate");
        CHECK(*keymap.commandFor(chord("Shift+Keyboard.F1"), ith::KeymapContext::PlayInEditor) ==
              "ith.play.free_mouse");
        CHECK(*keymap.commandFor(chord("Keyboard.F8"), ith::KeymapContext::PlayInEditor) ==
              "ith.play.possess_or_eject");
        // K keeps the simulation changes, in the Editor context (decision 7); a K bound to anything else
        // would have warned as a conflict above, and the Play In Editor context leaves it to the game.
        CHECK(*keymap.commandFor(chord("Keyboard.K")) == "ith.play.keep_simulation_changes");
        CHECK(keymap.chordsOf("ith.play.keep_simulation_changes") == std::vector<Chord>{chord("Keyboard.K")});
        CHECK(keymap.commandFor(chord("Keyboard.K"), ith::KeymapContext::PlayInEditor) == nullptr);
        CHECK(ith::contextOf("ith.play.keep_simulation_changes") == ith::KeymapContext::Editor);
        // The viewport's keys (ADR-0042 rule 29) name registered commands, so they warn no more.
        for (const auto& [key, id] :
             {std::pair{"Keyboard.F", "ith.viewport.focus_selection"},
              std::pair{"Keyboard.W", "ith.viewport.translate_tool"},
              std::pair{"Keyboard.E", "ith.viewport.rotate_tool"}, std::pair{"Keyboard.R", "ith.viewport.scale_tool"},
              std::pair{"Keyboard.Space", "ith.viewport.cycle_tool"},
              std::pair{"Keyboard.End", "ith.viewport.snap_to_floor"},
              std::pair{"Keyboard.G", "ith.viewport.game_view"}, std::pair{"Keyboard.F11", "ith.viewport.immersive"}}) {
            CAPTURE(id);
            CHECK(*keymap.commandFor(chord(key)) == id);
            CHECK(session.commands().find(id) != nullptr);
            CHECK(std::ranges::none_of(warnings, [&](const ith::Error& warning) {
                return warning.message == "no command " + std::string(id);
            }));
        }
        for (const ith::KeymapContext context :
             {ith::KeymapContext::Editor, ith::KeymapContext::PlayInEditor, ith::KeymapContext::Content}) {
            for (const auto& [bound, id] : keymap.bindings(context)) {
                CHECK_MESSAGE(session.commands().find(id) != nullptr, id);
            }
        }
    }

    TEST_CASE("the consumer runs commands by chord, letters by layout, and cancels an interaction on Escape") {
        EditorSession session(types());
        std::vector<ith::Error> warnings;
        const Keymap keymap =
            keymapOf(session, {R"({"ith.edit.undo": ["Ctrl+Keyboard.Z"], "ith.edit.select_all": ["Ctrl+Keyboard.A"],
                          "ith.edit.select_none": ["Keyboard.Escape"]})"},
                     warnings);
        ith::KeymapConsumer consumer(session, keymap);
        run(session, "ith.entity.create", R"({"label": "One"})");
        run(session, "ith.edit.select_none");

        // The key that prints "a" on this layout (Q on AZERTY) is the A of Ctrl + A.
        CHECK(consumer.onInputEvent(keyDown(Key::Q, U'a', KeyModifiers::Control)) == ith::InputReply::Consumed);
        CHECK(session.document().selection().entities().size() == 1);
        CHECK(consumer.onInputEvent(keyDown(Key::A, U'a')) == ith::InputReply::NotConsumed); // no Ctrl
        CHECK(consumer.onInputEvent(keyDown(Key::Escape, 0, KeyModifiers::NumLock)) == ith::InputReply::Consumed);
        CHECK(session.document().selection().empty());

        // A repeat runs nothing; Ctrl + Z undoes the create.
        CHECK(consumer.onInputEvent(keyDown(Key::Z, U'z', KeyModifiers::Control, true)) ==
              ith::InputReply::NotConsumed);
        CHECK(consumer.onInputEvent(keyDown(Key::Z, U'z', KeyModifiers::Control)) == ith::InputReply::Consumed);
        CHECK(session.document().world().entityCount() == 0);
        // Nothing left to undo: the press is still consumed, and the refusal kept.
        CHECK(consumer.onInputEvent(keyDown(Key::Z, U'z', KeyModifiers::Control)) == ith::InputReply::Consumed);
        CHECK_FALSE(consumer.lastResult().hasValue());

        // During an interaction Escape cancels it, and other chords pass on.
        run(session, "ith.entity.create", R"({"label": "Two"})");
        const std::string before = ith::test::editor::levelText(session);
        ith::TransactionScope drag = session.document().transactions().begin("Drag");
        REQUIRE(session.execute("ith.edit.rename", json(R"({"label": "Dragged"})")).error().code ==
                "editor.command.interaction_open");
        CHECK(consumer.onInputEvent(keyDown(Key::A, U'a', KeyModifiers::Control)) == ith::InputReply::NotConsumed);
        CHECK(consumer.onInputEvent(keyDown(Key::Escape, 0)) == ith::InputReply::Consumed);
        CHECK_FALSE(drag.active());
        CHECK_FALSE(session.document().transactions().isOpen());
        CHECK(ith::test::editor::levelText(session) == before);
        drag.commit(); // does nothing: the transaction was cancelled
        CHECK(session.document().history().entries().size() == 1);
    }

    TEST_CASE("an external cancel rolls back what the interaction applied") {
        EditorSession session(types());
        run(session, "ith.entity.create", R"({"label": "Three", "components": {"ith::test::editor::Lamp": {}}})");
        const std::string before = ith::test::editor::levelText(session);
        ith::Transactions& transactions = session.document().transactions();
        {
            ith::TransactionScope drag = transactions.begin("Drag");
            ith::TransactionScope inner = transactions.begin("Inner");
            REQUIRE(session.commands().execute(session, "ith.edit.select_all").error().code ==
                    "editor.command.interaction_open");
            transactions.cancelOpen();
            CHECK_FALSE(inner.active());
            CHECK_FALSE(drag.active());
            CHECK_FALSE(transactions.isOpen());
        }
        CHECK(ith::test::editor::levelText(session) == before);
        CHECK(transactions.begin("Next").active());
    }
}

TEST_SUITE("editor_core.keymap") {
    TEST_CASE("a shortcut invokes a command's prompt, which gathers the input; without one it executes") {
        EditorSession session(types());
        std::vector<ith::Error> warnings;
        const Keymap keymap = keymapOf(session, {R"({"ith.edit.rename": ["Keyboard.F2"]})"}, warnings);
        ith::KeymapConsumer consumer(session, keymap);
        run(session, "ith.entity.create", R"({"label": "One"})");

        // Without a prompt, F2 runs Rename without a label, which it refuses.
        CHECK(consumer.onInputEvent(keyDown(Key::F2, 0)) == ith::InputReply::Consumed);
        REQUIRE_FALSE(consumer.lastResult().hasValue());
        CHECK(consumer.lastResult().error().code == "editor.command.invalid_args");

        // A prompt that starts runs instead; one that declines lets the command run.
        int prompted = 0;
        bool starts = true;
        session.commands().setPrompt("ith.edit.rename", [&](EditorSession&) {
            ++prompted;
            return starts;
        });
        CHECK(consumer.onInputEvent(keyDown(Key::F2, 0)) == ith::InputReply::Consumed);
        CHECK(consumer.lastResult().hasValue());
        CHECK(prompted == 1);
        starts = false;
        CHECK(consumer.onInputEvent(keyDown(Key::F2, 0)) == ith::InputReply::Consumed);
        CHECK(prompted == 2);
        CHECK(consumer.lastResult().error().code == "editor.command.invalid_args");

        // The checks come first: while playing the prompt doesn't start.
        session.document().setPlaying(true);
        starts = true;
        CHECK_FALSE(session.commands().invoke(session, "ith.edit.rename").hasValue());
        CHECK(prompted == 2);
        session.document().setPlaying(false);

        // An empty prompt removes it; automation's execute never prompts.
        session.commands().setPrompt("ith.edit.rename", {});
        CHECK(session.commands().invoke(session, "ith.edit.rename").error().code == "editor.command.invalid_args");
        CHECK(session.commands().invoke(session, "ith.no_such").error().code == "editor.command.unknown");
        run(session, "ith.edit.rename", R"({"label": "Two"})");
        CHECK(prompted == 2);
    }
}
