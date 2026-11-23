#pragma once

// The editor keymap: shortcut chords bound to command ids (ADR-0029 rules 7 and 16, ADR-0035
// rule 21, ADR-0037 rule 4).
//
//   std::vector<ith::Error> warnings;
//   ith::Keymap keymap = ith::Keymap::load(config, session.commands(), warnings);
//   ith::KeymapConsumer shortcuts(session, keymap);   // added to each editor window's input stack
//
// The keymap is the "commands" section of Keymap.json in the layered configuration: a command id
// maps to its chords, and [] unbinds it in a higher layer. A chord is a stable control name of a
// key (ADR-0013) after optional "Ctrl+", "Shift+" and "Alt+": "Ctrl+Keyboard.Z", "Keyboard.F2".
// The engine layer ships the ADR-0008 preset; the rebinding screen comes later.
//
// Loading reports, without failing:
// - "editor.keymap.invalid_chord": a chord that doesn't parse, or a value that is not an array of
//   chords; the chord is skipped;
// - "editor.keymap.unknown_command": a command id the registry doesn't have (yet: viewport and
//   Play In Editor commands are registered by later editor modules); its chords are kept, so the
//   command works once it is registered;
// - "editor.keymap.conflict": a chord bound to two commands of one context. The command bound in
//   the higher layer wins; within one layer, the lower id.
//
// Two contexts: the Play In Editor keys of ADR-0008 rule 4 (kPlayInEditorCommands: stop, free the
// mouse, possess or eject), which the Play In Editor consumer handles while a game runs (ADR-0029
// rule 7), and every other command, which the keymap consumer handles. So Escape can stop play
// and, in the editor, clear the selection.

#include "ith/foundation/Expected.h"
#include "ith/input/Input.h"
#include "ith/platform_events/InputCodes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ith {

class CommandRegistry;
class Config;
class EditorSession;
struct Event;

namespace serialization {
class Value;
}

// The commands bound in the Play In Editor context.
inline constexpr std::array<std::string_view, 3> kPlayInEditorCommands = {"ith.play.stop", "ith.play.free_mouse",
                                                                          "ith.play.possess_or_eject"};

enum class KeymapContext : std::uint8_t {
    Editor,
    PlayInEditor,
};

// The context of the command `id`.
[[nodiscard]] KeymapContext contextOf(std::string_view id) noexcept;

// A key and the modifiers held with it: Ctrl, Shift and Alt only.
struct Chord {
    Key key = Key::None;
    KeyModifiers modifiers = KeyModifiers::None;

    friend bool operator==(const Chord&, const Chord&) = default;
    friend auto operator<=>(const Chord&, const Chord&) = default;
};

// Parses "Ctrl+Shift+Keyboard.S". Fails with "editor.keymap.invalid_chord": an unknown or repeated
// modifier, or a control that is not a key.
[[nodiscard]] Expected<Chord> parseChord(std::string_view text);

// The text form parseChord reads, modifiers in the order Ctrl, Shift, Alt.
[[nodiscard]] std::string toString(const Chord& chord);

class Keymap {
public:
    // Reads Keymap.json from each layer of `config`, lowest first.
    [[nodiscard]] static Keymap load(const Config& config, const CommandRegistry& registry,
                                     std::vector<Error>& warnings);

    // The keymap of the "commands" sections of each layer, lowest first; a null section is a layer
    // without the file.
    [[nodiscard]] static Keymap fromLayers(std::span<const serialization::Value* const> layers,
                                           const CommandRegistry& registry, std::vector<Error>& warnings);

    // The command bound to `chord` in `context`, or null.
    [[nodiscard]] const std::string* commandFor(const Chord& chord,
                                                KeymapContext context = KeymapContext::Editor) const;

    // The chords of `command`, in the order the keymap lists them; empty when unbound.
    [[nodiscard]] std::vector<Chord> chordsOf(std::string_view command) const;

    // Every binding of `context`, by chord.
    [[nodiscard]] const std::map<Chord, std::string>&
    bindings(KeymapContext context = KeymapContext::Editor) const noexcept {
        return m_bindings[static_cast<std::size_t>(context)];
    }

private:
    std::array<std::map<Chord, std::string>, 2> m_bindings;
    std::map<std::string, std::vector<Chord>, std::less<>> m_chords;
};

// The keymap consumer of editor_core (ADR-0029 rule 7): turns a key press with its modifiers into
// the bound command, invoked as a user gesture (CommandRegistry::invoke, so a prompt can ask for its
// input). Letter keys follow the user's layout (Event::layoutKey), so Ctrl + Z is the
// key that prints Z; other keys are physical keys. A press that matches a binding is consumed, also
// when the command refuses to run (its error is kept in lastResult()). While an interaction holds a
// transaction open, Escape cancels it (Transactions::cancelOpen) and other chords are left to the
// consumers below. Key repeats run nothing. The keymap must outlive the consumer.
class KeymapConsumer final : public InputConsumer {
public:
    KeymapConsumer(EditorSession& session, const Keymap& keymap) noexcept : m_session(&session), m_keymap(&keymap) {}

    InputReply onInputEvent(const Event& event) override;

    // Points the consumer at a reloaded keymap.
    void setKeymap(const Keymap& keymap) noexcept { m_keymap = &keymap; }

    // The error of the last shortcut whose command refused to run, if any.
    [[nodiscard]] const Expected<void>& lastResult() const noexcept { return m_lastResult; }

private:
    EditorSession* m_session;
    const Keymap* m_keymap;
    Expected<void> m_lastResult;
};

} // namespace ith
