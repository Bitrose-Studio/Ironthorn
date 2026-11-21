#include "ith/input_actions/InputBindings.h"

#include "ith/serialization/TextArchive.h"
#include "ith/serialization/Value.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ith {
namespace {

using serialization::Value;

serialization::Error makeIssue(std::string code, std::string message, std::string_view source, std::string path) {
    return serialization::Error{
        .code = std::move(code), .message = std::move(message), .source = std::string(source), .path = std::move(path)};
}

void warn(const InputBindingsReadOptions& options, std::string code, std::string message, std::string path) {
    if (options.warnings != nullptr) {
        options.warnings->push_back(makeIssue(std::move(code), std::move(message), options.source, std::move(path)));
    }
}

// Keeps a list sorted by name, replacing an entry with the same name.
template <typename T>
void putSorted(std::vector<T>& list, T entry) {
    const auto at = std::ranges::lower_bound(list, entry.name, {}, &T::name);
    if (at != list.end() && at->name == entry.name) {
        *at = std::move(entry);
    } else {
        list.insert(at, std::move(entry));
    }
}

void putSlot(std::vector<InputSlotBinding>& list, InputSlotBinding entry) {
    const auto at = std::ranges::lower_bound(list, entry.slot, {}, &InputSlotBinding::slot);
    if (at != list.end() && at->slot == entry.slot) {
        *at = std::move(entry);
    } else {
        list.insert(at, std::move(entry));
    }
}

template <typename T, typename Key>
const T* findSorted(const std::vector<T>& list, std::string_view name, Key key) {
    const auto at = std::ranges::lower_bound(list, name, {}, key);
    return at != list.end() && std::invoke(key, *at) == name ? &*at : nullptr;
}

std::variant<float, bool> declaredDefault(const InputSettingDecl& declared) {
    if (declared.type == InputSettingType::Bool) {
        return declared.defaultBool;
    }
    return declared.defaultFloat;
}

const InputBindingProfile* findProfile(const InputBindings& bindings, std::string_view name) {
    return findSorted(bindings.profiles, name, &InputBindingProfile::name);
}

// Why `key` can't go into `slot`, or nullopt when it can (section 3.11): it must be of the slot's
// device family and of a compatible raw kind. Unbound (nullopt) always fits.
std::optional<Error> checkSlotKey(const InputSlotInfo& slot, const std::optional<Control>& key) {
    if (!key.has_value()) {
        return std::nullopt;
    }
    if (deviceFamily(*key) != slot.family) {
        return Error{.code = "input_actions.wrong_device_family",
                     .message = "the slot '" + slot.name + "' takes " +
                                (slot.family == InputDeviceFamily::Gamepad ? "gamepad" : "keyboard and mouse") +
                                " keys, not '" + std::string(controlName(*key)) + "'"};
    }
    const InputRawKind kind = rawKind(*key);
    const bool compatible = slot.rawKind == InputRawKind::Digital
                                ? kind == InputRawKind::Digital || kind == InputRawKind::Axis1
                                : kind == slot.rawKind;
    if (!compatible) {
        return Error{.code = "input_actions.incompatible_key",
                     .message = "the key '" + std::string(controlName(*key)) +
                                "' reports another kind of value than the slot '" + slot.name + "' takes"};
    }
    return std::nullopt;
}

// A float setting clamped to its declared range.
float clampSetting(float value, const InputSettingDecl& declared) {
    return std::clamp(value, declared.min, std::max(declared.min, declared.max));
}

// One setting of a profile: a boolean, or a number within the range of a float.
Expected<InputSettingValue, serialization::Error> readSetting(const Value::Member& setting, const std::string& path,
                                                              const InputBindingsReadOptions& options) {
    InputSettingValue entry{.name = setting.key};
    if (setting.value.kind() == Value::Kind::Bool) {
        entry.value = setting.value.asBool();
        return entry;
    }
    const std::optional<double> number = setting.value.toNumber<double>();
    if (!number.has_value()) {
        return makeUnexpected(
            makeIssue("serialization.wrong_type", "a setting is a number or true or false", options.source, path));
    }
    // Checked before the cast: a double beyond the range of float can't be converted.
    const bool fits = std::abs(*number) <= static_cast<double>(std::numeric_limits<float>::max());
    if (!fits) {
        return makeUnexpected(makeIssue("serialization.out_of_range", "the setting is beyond the range of a float",
                                        options.source, path));
    }
    entry.value = static_cast<float>(*number);
    return entry;
}

Expected<InputBindingProfile, serialization::Error>
readProfile(const Value& value, std::string name, const std::string& path, const InputBindingsReadOptions& options) {
    if (value.kind() != Value::Kind::Object) {
        return makeUnexpected(makeIssue("serialization.wrong_type", "a profile is an object", options.source, path));
    }
    InputBindingProfile profile{.name = std::move(name)};
    for (const Value::Member& member : value.members()) {
        const std::string memberPath = path + "." + member.key;
        if (member.key == "slots") {
            if (member.value.kind() != Value::Kind::Object) {
                return makeUnexpected(makeIssue("serialization.wrong_type", "\"slots\" is an object of slot names",
                                                options.source, memberPath));
            }
            for (const Value::Member& slot : member.value.members()) {
                const std::string slotPath = memberPath + "." + slot.key;
                if (slot.value.kind() == Value::Kind::Null) {
                    putSlot(profile.slots, InputSlotBinding{.slot = slot.key});
                    continue;
                }
                if (slot.value.kind() != Value::Kind::String) {
                    return makeUnexpected(makeIssue("serialization.wrong_type", "a slot is a key name or null",
                                                    options.source, slotPath));
                }
                const std::optional<Control> key = findControl(slot.value.asString());
                if (!key.has_value()) {
                    warn(options, "input_actions.unknown_key",
                         "the unknown key '" + slot.value.asString() + "' is ignored", slotPath);
                    continue;
                }
                putSlot(profile.slots, InputSlotBinding{.slot = slot.key, .key = key});
            }
        } else if (member.key == "settings") {
            if (member.value.kind() != Value::Kind::Object) {
                return makeUnexpected(makeIssue("serialization.wrong_type",
                                                "\"settings\" is an object of setting names", options.source,
                                                memberPath));
            }
            for (const Value::Member& setting : member.value.members()) {
                Expected<InputSettingValue, serialization::Error> entry =
                    readSetting(setting, memberPath + "." + setting.key, options);
                if (!entry) {
                    return makeUnexpected(std::move(entry).error());
                }
                putSorted(profile.settings, std::move(*entry));
            }
        } else {
            warn(options, "serialization.unknown_field", "unknown field '" + member.key + "' is dropped", memberPath);
        }
    }
    return profile;
}

} // namespace

Expected<InputBindings, serialization::Error> readInputBindings(std::string_view text,
                                                                const InputBindingsReadOptions& options) {
    Expected<Value, serialization::Error> data = serialization::readDocument(
        text, kInputBindingsFormat,
        {.source = options.source, .syntax = serialization::JsonSyntax::Config, .warnings = options.warnings});
    if (!data) {
        return makeUnexpected(std::move(data).error());
    }
    if (data->kind() != Value::Kind::Object) {
        return makeUnexpected(makeIssue("serialization.wrong_type", "the data is an object", options.source, "$"));
    }
    InputBindings bindings;
    for (const Value::Member& member : data->members()) {
        if (member.key != "profiles") {
            warn(options, "serialization.unknown_field", "unknown field '" + member.key + "' is dropped",
                 "$." + member.key);
            continue;
        }
        if (member.value.kind() != Value::Kind::Object) {
            return makeUnexpected(makeIssue("serialization.wrong_type", "\"profiles\" is an object of profile names",
                                            options.source, "$.profiles"));
        }
        for (const Value::Member& profile : member.value.members()) {
            Expected<InputBindingProfile, serialization::Error> read =
                readProfile(profile.value, profile.key, "$.profiles." + profile.key, options);
            if (!read) {
                return makeUnexpected(std::move(read).error());
            }
            putSorted(bindings.profiles, std::move(*read));
        }
    }
    return bindings;
}

Expected<std::string, serialization::Error> writeInputBindings(const InputBindings& bindings) {
    Value profiles = Value::object();
    for (const InputBindingProfile& profile : bindings.profiles) {
        Value slots = Value::object();
        for (const InputSlotBinding& slot : profile.slots) {
            slots.set(slot.slot, slot.key ? Value::string(std::string(controlName(*slot.key))) : Value());
        }
        Value settings = Value::object();
        for (const InputSettingValue& setting : profile.settings) {
            if (const bool* flag = std::get_if<bool>(&setting.value)) {
                settings.set(setting.name, Value::boolean(*flag));
            } else {
                settings.set(setting.name, Value::real(std::get<float>(setting.value)));
            }
        }
        Value entry = Value::object();
        entry.set("slots", std::move(slots));
        entry.set("settings", std::move(settings));
        profiles.set(profile.name, std::move(entry));
    }
    Value data = Value::object();
    data.set("profiles", std::move(profiles));
    return serialization::writeDocument(kInputBindingsFormat, data);
}

void InputBindingConfig::setLayer(InputBindingLayer layer, InputBindings bindings, std::string_view source,
                                  std::vector<serialization::Error>* warnings) {
    const auto report = [&](std::string code, std::string message) {
        if (warnings != nullptr) {
            warnings->push_back(makeIssue(std::move(code), std::move(message), source, {}));
        }
    };
    for (InputBindingProfile& profile : bindings.profiles) {
        std::erase_if(profile.slots, [&](const InputSlotBinding& slot) {
            const std::optional<InputSlotInfo> info = m_library->findSlot(slot.slot);
            if (!info.has_value()) {
                report("input_actions.unknown_slot",
                       "the unknown slot '" + slot.slot + "' of profile '" + profile.name + "' is ignored");
                return true;
            }
            // The checks of rebind: a hand-edited file can't bind a key a player couldn't.
            if (const std::optional<Error> refused = checkSlotKey(*info, slot.key)) {
                report(refused->code, refused->message + " (profile '" + profile.name + "'); it is ignored");
                return true;
            }
            return false;
        });
        std::erase_if(profile.settings, [&](InputSettingValue& setting) {
            const InputSettingDecl* declared = m_library->findSetting(setting.name);
            if (declared == nullptr) {
                report("input_actions.unknown_setting",
                       "the unknown setting '" + setting.name + "' of profile '" + profile.name + "' is ignored");
                return true;
            }
            if (std::holds_alternative<bool>(setting.value) != (declared->type == InputSettingType::Bool)) {
                report("input_actions.wrong_setting_type",
                       "the setting '" + setting.name + "' of profile '" + profile.name + "' has the wrong type");
                return true;
            }
            if (float* number = std::get_if<float>(&setting.value)) {
                *number = clampSetting(*number, *declared);
            }
            return false;
        });
        // Bindings built by code may be in any order; lookups need them sorted by name.
        std::ranges::sort(profile.slots, {}, &InputSlotBinding::slot);
        std::ranges::sort(profile.settings, {}, &InputSettingValue::name);
    }
    std::ranges::sort(bindings.profiles, {}, &InputBindingProfile::name);
    m_layers[static_cast<std::size_t>(layer)] = std::move(bindings);
    ++m_version;
}

const InputSlotBinding* InputBindingConfig::findSlot(std::string_view profile, std::string_view slot,
                                                     std::size_t top) const {
    for (std::size_t layer = top; layer-- > 0;) {
        if (const InputBindingProfile* found = findProfile(m_layers[layer], profile)) {
            if (const InputSlotBinding* binding = findSorted(found->slots, slot, &InputSlotBinding::slot)) {
                return binding;
            }
        }
    }
    return nullptr;
}

const InputSettingValue* InputBindingConfig::findSetting(std::string_view profile, std::string_view name,
                                                         std::size_t top) const {
    for (std::size_t layer = top; layer-- > 0;) {
        if (const InputBindingProfile* found = findProfile(m_layers[layer], profile)) {
            if (const InputSettingValue* setting = findSorted(found->settings, name, &InputSettingValue::name)) {
                return setting;
            }
        }
    }
    return nullptr;
}

std::optional<Control> InputBindingConfig::key(std::string_view profile, std::string_view slot) const {
    if (const InputSlotBinding* binding = findSlot(profile, slot, m_layers.size())) {
        return binding->key;
    }
    const std::optional<InputSlotInfo> info = m_library->findSlot(slot);
    return info ? info->defaultKey : std::nullopt;
}

Expected<void> InputBindingConfig::rebind(std::string_view profile, std::string_view slot, std::optional<Control> key) {
    const std::optional<InputSlotInfo> info = m_library->findSlot(slot);
    if (!info.has_value()) {
        return makeError("input_actions.unknown_slot", "there is no slot '" + std::string(slot) + "'");
    }
    if (std::optional<Error> refused = checkSlotKey(*info, key)) {
        return makeUnexpected(std::move(*refused));
    }
    constexpr auto kUser = static_cast<std::size_t>(InputBindingLayer::User);
    const InputSlotBinding* lower = findSlot(profile, slot, kUser);
    const std::optional<Control> lowerKey = lower != nullptr ? lower->key : info->defaultKey;
    InputBindingProfile& user = userProfile(profile);
    if (key == lowerKey) {
        std::erase_if(user.slots, [slot](const InputSlotBinding& binding) { return binding.slot == slot; });
        dropEmptyUserProfiles();
    } else {
        putSlot(user.slots, InputSlotBinding{.slot = std::string(slot), .key = key});
    }
    ++m_version;
    return {};
}

std::vector<InputBindingConflict> InputBindingConfig::conflicts(std::string_view profile, std::string_view slot,
                                                                const Control& key) const {
    std::vector<InputBindingConflict> result;
    const std::optional<InputSlotInfo> info = m_library->findSlot(slot);
    const InputDeviceFamily family = info ? info->family : deviceFamily(key);
    for (InputSlotInfo& other : m_library->slots()) {
        if (other.name == slot || other.family != family) {
            continue;
        }
        if (this->key(profile, other.name) == key) {
            result.push_back(InputBindingConflict{
                .slot = other.name, .action = other.action, .contexts = std::move(other.contexts)});
        }
    }
    return result;
}

std::optional<std::variant<float, bool>> InputBindingConfig::setting(std::string_view profile,
                                                                     std::string_view name) const {
    const InputSettingDecl* declared = m_library->findSetting(name);
    if (declared == nullptr) {
        return std::nullopt;
    }
    if (const InputSettingValue* value = findSetting(profile, name, m_layers.size())) {
        return value->value;
    }
    return declaredDefault(*declared);
}

Expected<void> InputBindingConfig::setSetting(std::string_view profile, std::string_view name,
                                              std::variant<float, bool> value) {
    const InputSettingDecl* declared = m_library->findSetting(name);
    if (declared == nullptr) {
        return makeError("input_actions.unknown_setting", "there is no input setting '" + std::string(name) + "'");
    }
    if (std::holds_alternative<bool>(value) != (declared->type == InputSettingType::Bool)) {
        return makeError("input_actions.wrong_setting_type",
                         "the input setting '" + std::string(name) + "' has another type");
    }
    if (float* number = std::get_if<float>(&value)) {
        *number = clampSetting(*number, *declared);
    }
    constexpr auto kUser = static_cast<std::size_t>(InputBindingLayer::User);
    const InputSettingValue* lower = findSetting(profile, name, kUser);
    const std::variant<float, bool> lowerValue = lower != nullptr ? lower->value : declaredDefault(*declared);
    InputBindingProfile& user = userProfile(profile);
    if (value == lowerValue) {
        std::erase_if(user.settings, [name](const InputSettingValue& setting) { return setting.name == name; });
        dropEmptyUserProfiles();
    } else {
        putSorted(user.settings, InputSettingValue{.name = std::string(name), .value = value});
    }
    ++m_version;
    return {};
}

void InputBindingConfig::resetProfile(std::string_view profile) {
    std::erase_if(m_layers[static_cast<std::size_t>(InputBindingLayer::User)].profiles,
                  [profile](const InputBindingProfile& entry) { return entry.name == profile; });
    ++m_version;
}

void InputBindingConfig::copyProfile(std::string_view from, std::string_view to) {
    // Only what differs from what the layers below the user layer give `to` is written.
    constexpr auto kUser = static_cast<std::size_t>(InputBindingLayer::User);
    InputBindingProfile copy{.name = std::string(to)};
    for (const InputSlotInfo& slot : m_library->slots()) {
        const std::optional<Control> value = key(from, slot.name);
        const InputSlotBinding* lower = findSlot(to, slot.name, kUser);
        if (value != (lower != nullptr ? lower->key : slot.defaultKey)) {
            putSlot(copy.slots, InputSlotBinding{.slot = slot.name, .key = value});
        }
    }
    for (const InputSettingDecl& declared : m_library->settings()) {
        const std::optional<std::variant<float, bool>> value = setting(from, declared.name);
        const InputSettingValue* lower = findSetting(to, declared.name, kUser);
        const std::variant<float, bool> lowerValue = lower != nullptr ? lower->value : declaredDefault(declared);
        if (value.has_value() && *value != lowerValue) {
            putSorted(copy.settings, InputSettingValue{.name = declared.name, .value = *value});
        }
    }
    std::vector<InputBindingProfile>& profiles = m_layers[kUser].profiles;
    std::erase_if(profiles, [to](const InputBindingProfile& entry) { return entry.name == to; });
    if (!copy.slots.empty() || !copy.settings.empty()) {
        putSorted(profiles, std::move(copy));
    }
    ++m_version;
}

InputBindingProfile& InputBindingConfig::userProfile(std::string_view profile) {
    std::vector<InputBindingProfile>& profiles = m_layers[static_cast<std::size_t>(InputBindingLayer::User)].profiles;
    const auto at = std::ranges::lower_bound(profiles, profile, {}, &InputBindingProfile::name);
    if (at != profiles.end() && at->name == profile) {
        return *at;
    }
    return *profiles.insert(at, InputBindingProfile{.name = std::string(profile)});
}

void InputBindingConfig::dropEmptyUserProfiles() {
    std::erase_if(m_layers[static_cast<std::size_t>(InputBindingLayer::User)].profiles,
                  [](const InputBindingProfile& profile) { return profile.slots.empty() && profile.settings.empty(); });
}

} // namespace ith
