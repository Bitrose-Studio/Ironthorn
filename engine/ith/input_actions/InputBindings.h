#pragma once

// Rebinding: binding profiles, the layered configuration that stores them, and the
// "ith.input_bindings" config document (docs/specs/input-actions.md, section 3.11; ADR-0013 rule
// 13).
//
//   ith::InputBindingConfig config(library);
//   std::vector<ith::Error> warnings;
//   const auto user = ith::readInputBindings(text, {.source = "user/input.json", .warnings = &warnings});
//   config.setLayer(ith::InputBindingLayer::User, *user, "user/input.json", &warnings);
//   if (const auto conflicts = config.conflicts("default", "jump.keyboard", ith::Key::F); conflicts.empty()) {
//       config.rebind("default", "jump.keyboard", ith::Key::F);
//   }
//   const auto saved = ith::writeInputBindings(config.layer(ith::InputBindingLayer::User));
//
// A slot's key is the authored key of its mappings unless a layer overrides it; the highest layer
// that overrides a slot or setting of a profile wins (engine, project, platform, user). Players'
// changes go to the user layer, only where they differ from the layers below. Where each layer's
// file lives is not decided yet (the spec's decision 6): these functions read and write document
// content only.
//
// Not thread-safe: one thread at a time.

#include "ith/foundation/Expected.h"
#include "ith/input/ControlNames.h"
#include "ith/input_actions/InputAssetLibrary.h"
#include "ith/serialization/Error.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ith {

// One slot override: a key, or unbound.
struct InputSlotBinding {
    std::string slot;
    // nullopt: the slot is unbound (no key).
    std::optional<Control> key;

    friend bool operator==(const InputSlotBinding&, const InputSlotBinding&) = default;
};

// One input setting value: a float or a boolean.
struct InputSettingValue {
    std::string name;
    std::variant<float, bool> value;

    friend bool operator==(const InputSettingValue&, const InputSettingValue&) = default;
};

// A named set of rebinds and input setting values. Slots and settings are kept sorted by name.
struct InputBindingProfile {
    std::string name;
    std::vector<InputSlotBinding> slots;
    std::vector<InputSettingValue> settings;

    friend bool operator==(const InputBindingProfile&, const InputBindingProfile&) = default;
};

// The content of one "ith.input_bindings" document: profiles, sorted by name.
struct InputBindings {
    std::vector<InputBindingProfile> profiles;

    friend bool operator==(const InputBindings&, const InputBindings&) = default;
};

inline constexpr serialization::DocumentFormat kInputBindingsFormat{.name = "ith.input_bindings", .version = 1};

// The profile players use when they have none set.
inline constexpr std::string_view kDefaultBindingProfile = "default";

struct InputBindingsReadOptions {
    // The file name, copied into errors and warnings.
    std::string_view source;
    // Receives the warnings: unknown document keys and fields, and unknown key names, whose
    // entries are ignored (ADR-0010 rule 13). Null to not collect them.
    std::vector<serialization::Error>* warnings = nullptr;
};

// Reads an "ith.input_bindings" config document (comments allowed):
//
//   {"profiles": {"default": {"slots": {"jump.keyboard": "Keyboard.Space", "crouch.gamepad": null},
//                             "settings": {"look_sensitivity": 1.4, "invert_look_y": true}}}}
//
// Fails with the readDocument errors, "serialization.wrong_type" for a value of the wrong JSON
// type and "serialization.out_of_range" for a number beyond the range of a float. Unknown slot and
// setting names can only be told by the assets; InputBindingConfig::setLayer reports them.
Expected<InputBindings, serialization::Error> readInputBindings(std::string_view text,
                                                                const InputBindingsReadOptions& options = {});

// Writes `bindings` as an "ith.input_bindings" document.
Expected<std::string, serialization::Error> writeInputBindings(const InputBindings& bindings);

// The layers of the configuration, lowest first (ADR-0008).
enum class InputBindingLayer : std::uint8_t {
    Engine,
    Project,
    Platform,
    User,
};

// Another slot that uses a key, from InputBindingConfig::conflicts.
struct InputBindingConflict {
    std::string slot;
    AssetId action;
    std::vector<AssetId> contexts;
};

// The layered bindings of every profile, resolved against the assets of a library.
class InputBindingConfig {
public:
    // `library` must outlive the config.
    explicit InputBindingConfig(const InputAssetLibrary& library) : m_library(&library) {}

    // Replaces a layer. Slots and settings the library doesn't know, keys rebind() would refuse
    // (another device family, an incompatible raw kind) and settings of the wrong type are dropped
    // with a warning naming `source` and the name. Float settings are clamped to their declared
    // range, and profiles, slots and settings are sorted by name, so bindings may be built by code.
    void setLayer(InputBindingLayer layer, InputBindings bindings, std::string_view source = {},
                  std::vector<serialization::Error>* warnings = nullptr);

    [[nodiscard]] const InputBindings& layer(InputBindingLayer layer) const {
        return m_layers[static_cast<std::size_t>(layer)];
    }

    // The effective key of `slot` in `profile`: the highest layer's override, else the authored
    // key. nullopt when unbound or when the slot doesn't exist.
    [[nodiscard]] std::optional<Control> key(std::string_view profile, std::string_view slot) const;

    // Rebinds `slot` of `profile` in the user layer, to `key` or unbound (nullopt), taking effect at
    // the next InputActions::update. Writes nothing when `key` is what the lower layers give, and
    // removes an older user override then. Fails with "input_actions.unknown_slot",
    // "input_actions.wrong_device_family" (a key of the other family) or
    // "input_actions.incompatible_key" (a digital slot takes digital or 1D keys, a 1D slot 1D keys,
    // a 2D slot 2D keys). Conflicts are not checked: ask conflicts() first.
    Expected<void> rebind(std::string_view profile, std::string_view slot, std::optional<Control> key);

    // Every other slot in the family of `slot` whose effective key in `profile` is `key`, by name,
    // with its action and contexts. It never resolves a conflict itself.
    [[nodiscard]] std::vector<InputBindingConflict> conflicts(std::string_view profile, std::string_view slot,
                                                              const Control& key) const;

    // The effective value of a setting in `profile`: the highest layer's value, else the declared
    // default. nullopt for an undeclared setting.
    [[nodiscard]] std::optional<std::variant<float, bool>> setting(std::string_view profile,
                                                                   std::string_view name) const;

    // Sets a setting of `profile` in the user layer (floats clamped to the declared range), only
    // where it differs from the lower layers. Fails with "input_actions.unknown_setting" or
    // "input_actions.wrong_setting_type".
    Expected<void> setSetting(std::string_view profile, std::string_view name, std::variant<float, bool> value);

    // Removes the user layer's overrides of `profile`, so the lower layers and the authored keys
    // and defaults apply again.
    void resetProfile(std::string_view profile);
    // Copies the effective slots and settings of `from` into the user layer as `to`, replacing
    // what the user layer had for `to`.
    void copyProfile(std::string_view from, std::string_view to);

    // Removes `profile` from the user layer; profiles of lower layers stay.
    void deleteProfile(std::string_view profile) { resetProfile(profile); }

    // Changes on every change of a layer, so InputActions rebuilds its mapping tables.
    [[nodiscard]] std::uint64_t version() const { return m_version; }

    [[nodiscard]] const InputAssetLibrary& library() const { return *m_library; }

private:
    // The slot override of the highest layer below `top` (exclusive), or nullptr.
    [[nodiscard]] const InputSlotBinding* findSlot(std::string_view profile, std::string_view slot,
                                                   std::size_t top) const;
    [[nodiscard]] const InputSettingValue* findSetting(std::string_view profile, std::string_view name,
                                                       std::size_t top) const;
    InputBindingProfile& userProfile(std::string_view profile);
    void dropEmptyUserProfiles();

    const InputAssetLibrary* m_library;
    std::array<InputBindings, 4> m_layers;
    std::uint64_t m_version = 1;
};

} // namespace ith
