#include "ith/editor_core/Keymap.h"

#include "ith/config/Config.h"
#include "ith/editor_core/CommandRegistry.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/input/ControlNames.h"
#include "ith/platform_events/Event.h"
#include "ith/serialization/TextArchive.h"
#include "ith/serialization/Value.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace ith {

namespace {

using serialization::Value;

constexpr KeyModifiers kChordModifiers = KeyModifiers::Control | KeyModifiers::Shift | KeyModifiers::Alt;

bool has(KeyModifiers set, KeyModifiers flag) {
    return (set & flag) == flag;
}

// One command's chords as the highest layer that lists the command gives them.
struct Listed {
    std::size_t layer = 0;
    std::vector<Chord> chords;
};

} // namespace

KeymapContext contextOf(std::string_view id) noexcept {
    const bool playKey =
        std::ranges::any_of(kPlayInEditorCommands, [&id](const std::string_view& command) { return command == id; });
    return playKey ? KeymapContext::PlayInEditor : KeymapContext::Editor;
}

Expected<Chord> parseChord(std::string_view text) {
    Chord chord;
    std::string_view rest = text;
    while (true) {
        const std::size_t plus = rest.find('+');
        if (plus == std::string_view::npos) {
            break;
        }
        const std::string_view name = rest.substr(0, plus);
        KeyModifiers modifier = KeyModifiers::None;
        if (name == "Ctrl") {
            modifier = KeyModifiers::Control;
        } else if (name == "Shift") {
            modifier = KeyModifiers::Shift;
        } else if (name == "Alt") {
            modifier = KeyModifiers::Alt;
        }
        if (modifier == KeyModifiers::None || has(chord.modifiers, modifier)) {
            return makeError("editor.keymap.invalid_chord", "\"" + std::string(text) +
                                                                "\": unknown or repeated modifier \"" +
                                                                std::string(name) + "\"");
        }
        chord.modifiers = chord.modifiers | modifier;
        rest = rest.substr(plus + 1);
    }
    const std::optional<Control> control = findControl(rest);
    const Key* key = control.has_value() ? std::get_if<Key>(&*control) : nullptr;
    if (key == nullptr || *key == Key::None) {
        return makeError("editor.keymap.invalid_chord",
                         "\"" + std::string(text) + "\": \"" + std::string(rest) + "\" is not a key");
    }
    chord.key = *key;
    return chord;
}

std::string toString(const Chord& chord) {
    std::string text;
    if (has(chord.modifiers, KeyModifiers::Control)) {
        text += "Ctrl+";
    }
    if (has(chord.modifiers, KeyModifiers::Shift)) {
        text += "Shift+";
    }
    if (has(chord.modifiers, KeyModifiers::Alt)) {
        text += "Alt+";
    }
    return text + std::string(controlName(chord.key));
}

Keymap Keymap::load(const Config& config, const CommandRegistry& registry, std::vector<Error>& warnings) {
    std::vector<std::optional<Value>> sections;
    for (const ConfigLayer& layer : config.layers()) {
        const std::string path = fs::joinPath(layer.directory, "Keymap.json");
        Expected<std::string> text = fs::readFile(path, kMaxConfigFileSize);
        if (!text) {
            if (text.error().code != "fs.not_found") {
                warnings.push_back(text.error());
            }
            sections.emplace_back();
            continue;
        }
        Expected<Value, serialization::Error> data = serialization::readDocument(
            *text, kConfigFormat,
            {.source = path, .syntax = serialization::JsonSyntax::Config, .maxSize = kMaxConfigFileSize});
        if (!data) {
            warnings.push_back(Error{.code = data.error().code, .message = data.error().toString()});
            sections.emplace_back();
            continue;
        }
        const Value* commands = data->kind() == Value::Kind::Object ? data->find("commands") : nullptr;
        sections.emplace_back(commands != nullptr ? std::optional<Value>(*commands) : std::nullopt);
    }
    std::vector<const Value*> layers;
    layers.reserve(sections.size());
    for (const std::optional<Value>& section : sections) {
        layers.push_back(section.has_value() ? &*section : nullptr);
    }
    return fromLayers(layers, registry, warnings);
}

Keymap Keymap::fromLayers(std::span<const Value* const> layers, const CommandRegistry& registry,
                          std::vector<Error>& warnings) {
    // RFC 7396 per command id: the highest layer that lists a command decides its chords.
    std::map<std::string, Listed, std::less<>> listed;
    for (std::size_t layer = 0; layer < layers.size(); ++layer) {
        const Value* section = layers[layer];
        if (section == nullptr || section->kind() != Value::Kind::Object) {
            continue;
        }
        for (const Value::Member& member : section->members()) {
            if (member.value.kind() == Value::Kind::Null) {
                listed.erase(member.key);
                continue;
            }
            if (member.value.kind() != Value::Kind::Array) {
                warnings.push_back(Error{.code = "editor.keymap.invalid_chord",
                                         .message = member.key + ": the chords must be an array of strings"});
                continue;
            }
            Listed entry{.layer = layer};
            for (const Value& element : member.value.elements()) {
                Expected<Chord> chord = element.kind() == Value::Kind::String
                                            ? parseChord(element.asString())
                                            : Expected<Chord>(makeError("editor.keymap.invalid_chord", "not a string"));
                if (!chord) {
                    warnings.push_back(
                        Error{.code = chord.error().code, .message = member.key + ": " + chord.error().message});
                    continue;
                }
                entry.chords.push_back(*chord);
            }
            listed.insert_or_assign(member.key, std::move(entry));
        }
    }
    // Bind the higher layers first, and within a layer the lower ids, so they win conflicts.
    std::vector<const std::pair<const std::string, Listed>*> order;
    order.reserve(listed.size());
    for (const auto& entry : listed) {
        order.push_back(&entry);
    }
    std::ranges::stable_sort(order, [](const auto* a, const auto* b) { return a->second.layer > b->second.layer; });
    Keymap keymap;
    for (const auto* entry : order) {
        const std::string& id = entry->first;
        if (registry.find(id) == nullptr && !entry->second.chords.empty()) {
            warnings.push_back(Error{.code = "editor.keymap.unknown_command", .message = "no command " + id});
        }
        std::vector<Chord>& chords = keymap.m_chords[id];
        for (const Chord& chord : entry->second.chords) {
            auto& bindings = keymap.m_bindings[static_cast<std::size_t>(contextOf(id))];
            const auto [bound, inserted] = bindings.try_emplace(chord, id);
            if (!inserted && bound->second != id) {
                warnings.push_back(Error{.code = "editor.keymap.conflict",
                                         .message = toString(chord) + " is bound to " + bound->second + " and to " +
                                                    id + "; " + bound->second + " keeps it"});
                continue;
            }
            chords.push_back(chord);
        }
    }
    return keymap;
}

const std::string* Keymap::commandFor(const Chord& chord, KeymapContext context) const {
    const std::map<Chord, std::string>& bindings = m_bindings[static_cast<std::size_t>(context)];
    const auto found = bindings.find(chord);
    return found != bindings.end() ? &found->second : nullptr;
}

std::vector<Chord> Keymap::chordsOf(std::string_view command) const {
    const auto found = m_chords.find(command);
    return found != m_chords.end() ? found->second : std::vector<Chord>();
}

InputReply KeymapConsumer::onInputEvent(const Event& event) {
    if (event.type != EventType::KeyDown || event.repeat) {
        return InputReply::NotConsumed;
    }
    Transactions& transactions = m_session->document().transactions();
    if (transactions.isOpen()) {
        if (event.key == Key::Escape) {
            transactions.cancelOpen();
            return InputReply::Consumed;
        }
        return InputReply::NotConsumed;
    }
    Chord chord{.key = event.key, .modifiers = event.modifiers & kChordModifiers};
    // Letters follow the layout: the key that prints Z is Z for shortcuts.
    if (event.layoutKey >= U'a' && event.layoutKey <= U'z') {
        chord.key = static_cast<Key>(static_cast<std::uint16_t>(Key::A) + (event.layoutKey - U'a'));
    }
    const std::string* command = m_keymap->commandFor(chord);
    if (command == nullptr) {
        return InputReply::NotConsumed;
    }
    Expected<serialization::Value> result = m_session->commands().invoke(*m_session, *command);
    m_lastResult = result ? Expected<void>() : Expected<void>(makeUnexpected(std::move(result).error()));
    return InputReply::Consumed;
}

} // namespace ith
