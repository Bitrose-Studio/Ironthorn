#include "ith/asset_database/Sidecar.h"

#include "ith/serialization/Json.h"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ith {
namespace {

using serialization::Value;

serialization::Error invalid(std::string message, std::string_view source = {}, std::string path = {}) {
    return serialization::Error{.code = "asset.sidecar_invalid",
                                .message = std::move(message),
                                .source = std::string(source),
                                .path = std::move(path)};
}

serialization::Error headerError(std::string code, std::string message, std::string_view source) {
    return serialization::Error{.code = std::move(code), .message = std::move(message), .source = std::string(source)};
}

bool isKeyTypeCharacter(char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_';
}

// Checks the rules writeSidecar documents; `sorted` also requires the table to be sorted.
std::optional<serialization::Error> checkRules(const Sidecar& sidecar, bool sorted, std::string_view source) {
    if (sidecar.id.isNil()) {
        return invalid("the asset id is nil", source);
    }
    if (sidecar.importer.empty()) {
        return invalid("the importer is empty", source, "$.importer");
    }
    if (sidecar.settings.kind() != Value::Kind::Object) {
        return invalid("the settings are not an object", source, "$.settings");
    }
    if (sidecar.overrides.kind() != Value::Kind::Object) {
        return invalid("the overrides are not an object", source, "$.overrides");
    }
    std::unordered_set<std::string_view> keys;
    std::unordered_set<SubAssetId> ids;
    for (std::size_t index = 0; index < sidecar.subAssets.size(); ++index) {
        const SubAssetEntry& entry = sidecar.subAssets[index];
        const std::string path = "$.subAssets[" + std::to_string(index) + "]";
        if (!isValidSourceKey(entry.key)) {
            return invalid("'" + entry.key + "' is not a valid source key", source, path + ".key");
        }
        if (!keys.insert(entry.key).second) {
            return invalid("the key '" + entry.key + "' is given twice", source, path + ".key");
        }
        if (entry.id.isNil()) {
            return invalid("the sub-asset id is nil", source, path + ".id");
        }
        if (!ids.insert(entry.id).second) {
            return invalid("the sub-asset id " + entry.id.toString() + " is given twice", source, path + ".id");
        }
        if (sorted && index > 0 && !(sidecar.subAssets[index - 1].key < entry.key)) {
            return invalid("the sub-asset table is not sorted by key", source, path + ".key");
        }
    }
    return std::nullopt;
}

bool lessByKey(const SubAssetEntry& a, const SubAssetEntry& b) {
    return a.key < b.key;
}

// The path of a member; header keys (an empty `path`) are outside the data and have none.
std::string memberPath(const std::string& path, const std::string& key) {
    return path.empty() ? std::string() : path + "." + key;
}

// Reads the members of an object, calling `read(key, value)` for each known key and warning about
// the others. Returns the first error `read` reports.
template <typename Read>
std::optional<serialization::Error> readMembers(const Value& object, std::initializer_list<std::string_view> known,
                                                const std::string& path, std::string_view source,
                                                std::vector<serialization::Error>* warnings, const Read& read) {
    std::unordered_set<std::string_view> seen;
    for (const Value::Member& member : object.members()) {
        if (std::ranges::find(known, member.key) == known.end()) {
            if (warnings != nullptr) {
                warnings->push_back(serialization::Error{.code = "serialization.unknown_field",
                                                         .message = "unknown key '" + member.key + "' is dropped",
                                                         .source = std::string(source),
                                                         .path = memberPath(path, member.key)});
            }
            continue;
        }
        if (!seen.insert(member.key).second) {
            return serialization::Error{.code = "serialization.duplicate_field",
                                        .message = "the key '" + member.key + "' is given twice",
                                        .source = std::string(source),
                                        .path = memberPath(path, member.key)};
        }
        if (std::optional<serialization::Error> error = read(member.key, member.value)) {
            return error;
        }
    }
    return std::nullopt;
}

std::optional<SubAssetId> parseSubAssetId(const Value& value) {
    if (value.kind() != Value::Kind::String) {
        return std::nullopt;
    }
    Expected<SubAssetId> id = SubAssetId::parse(value.asString());
    if (!id) {
        return std::nullopt;
    }
    return *id;
}

std::optional<serialization::Error> readSubAsset(const Value& value, const std::string& path, std::string_view source,
                                                 std::vector<serialization::Error>* warnings, SubAssetEntry& entry) {
    if (value.kind() != Value::Kind::Object) {
        return invalid("a sub-asset is not an object", source, path);
    }
    bool hasKey = false;
    bool hasId = false;
    std::optional<serialization::Error> error =
        readMembers(value, {"key", "id", "missing"}, path, source, warnings,
                    [&](std::string_view key, const Value& member) -> std::optional<serialization::Error> {
                        if (key == "key") {
                            if (member.kind() != Value::Kind::String) {
                                return invalid("the key is not a string", source, path + ".key");
                            }
                            entry.key = member.asString();
                            hasKey = true;
                        } else if (key == "id") {
                            const std::optional<SubAssetId> id = parseSubAssetId(member);
                            if (!id) {
                                return invalid("the id is not a UUID", source, path + ".id");
                            }
                            entry.id = *id;
                            hasId = true;
                        } else {
                            if (member.kind() != Value::Kind::Bool) {
                                return invalid("'missing' is not a boolean", source, path + ".missing");
                            }
                            entry.missing = member.asBool();
                        }
                        return std::nullopt;
                    });
    if (error) {
        return error;
    }
    if (!hasKey || !hasId) {
        return invalid("a sub-asset needs a key and an id", source, path);
    }
    return std::nullopt;
}

std::optional<serialization::Error> readData(const Value& data, std::string_view source,
                                             std::vector<serialization::Error>* warnings, Sidecar& sidecar) {
    if (data.kind() != Value::Kind::Object) {
        return invalid("the data is not an object", source, "$");
    }
    bool hasImporter = false;
    bool hasVersion = false;
    std::optional<serialization::Error> error =
        readMembers(data, {"importer", "importerVersion", "settings", "subAssets", "overrides"}, "$", source, warnings,
                    [&](std::string_view key, const Value& member) -> std::optional<serialization::Error> {
                        if (key == "importer") {
                            if (member.kind() != Value::Kind::String) {
                                return invalid("the importer is not a string", source, "$.importer");
                            }
                            sidecar.importer = member.asString();
                            hasImporter = true;
                        } else if (key == "importerVersion") {
                            const std::optional<std::uint32_t> version = member.toNumber<std::uint32_t>();
                            if (!version) {
                                return invalid("the importer version is not a 32-bit unsigned integer", source,
                                               "$.importerVersion");
                            }
                            sidecar.importerVersion = *version;
                            hasVersion = true;
                        } else if (key == "settings") {
                            sidecar.settings = member;
                        } else if (key == "overrides") {
                            sidecar.overrides = member;
                        } else {
                            if (member.kind() != Value::Kind::Array) {
                                return invalid("the sub-assets are not an array", source, "$.subAssets");
                            }
                            sidecar.subAssets.resize(member.elements().size());
                            for (std::size_t index = 0; index < member.elements().size(); ++index) {
                                if (std::optional<serialization::Error> entryError = readSubAsset(
                                        member.elements()[index], "$.subAssets[" + std::to_string(index) + "]", source,
                                        warnings, sidecar.subAssets[index])) {
                                    return entryError;
                                }
                            }
                        }
                        return std::nullopt;
                    });
    if (error) {
        return error;
    }
    if (!hasImporter || !hasVersion) {
        return invalid("the importer and its version are required", source, "$");
    }
    return std::nullopt;
}

} // namespace

bool isValidSourceKey(std::string_view key) noexcept {
    const std::size_t colon = key.find(':');
    if (colon == 0 || colon == std::string_view::npos || colon + 1 == key.size()) {
        return false;
    }
    return std::all_of(key.begin(), key.begin() + static_cast<std::ptrdiff_t>(colon), isKeyTypeCharacter);
}

Expected<std::string, serialization::Error> writeSidecar(const Sidecar& sidecar) {
    if (std::optional<serialization::Error> error = checkRules(sidecar, true, {})) {
        return makeUnexpected(std::move(*error));
    }
    Value subAssets = Value::array();
    for (const SubAssetEntry& entry : sidecar.subAssets) {
        Value row = Value::object();
        row.set("key", Value::string(entry.key));
        row.set("id", Value::string(entry.id.toString()));
        row.set("missing", Value::boolean(entry.missing));
        subAssets.elements().push_back(std::move(row));
    }
    Value data = Value::object();
    data.set("importer", Value::string(sidecar.importer));
    data.set("importerVersion", Value::unsignedInteger(sidecar.importerVersion));
    data.set("settings", sidecar.settings);
    data.set("subAssets", std::move(subAssets));
    data.set("overrides", sidecar.overrides);

    Value document = Value::object();
    document.set("format", Value::string(std::string(kSidecarFormat.name)));
    document.set("version", Value::unsignedInteger(kSidecarFormat.version));
    document.set("id", Value::string(sidecar.id.toString()));
    document.set("data", std::move(data));
    return serialization::writeJson(document);
}

Expected<Sidecar, serialization::Error> readSidecar(std::string_view text, std::string_view source,
                                                    std::vector<serialization::Error>* warnings) {
    Expected<Value, serialization::Error> document =
        serialization::parseJson(text, {.source = source, .maxSize = kMaxSidecarSize});
    if (!document) {
        return makeUnexpected(std::move(document).error());
    }
    constexpr const char* kNotADocument = R"(a sidecar is an object with "format", "version", "id" and "data")";
    if (document->kind() != Value::Kind::Object) {
        return makeUnexpected(headerError("serialization.not_a_document", kNotADocument, source));
    }

    const Value* format = nullptr;
    const Value* version = nullptr;
    const Value* id = nullptr;
    std::optional<serialization::Error> error =
        readMembers(*document, {"format", "version", "id", "data"}, "", source, warnings,
                    [&](std::string_view key, const Value& member) -> std::optional<serialization::Error> {
                        if (key == "format") {
                            format = &member;
                        } else if (key == "version") {
                            version = &member;
                        } else if (key == "id") {
                            id = &member;
                        }
                        return std::nullopt;
                    });
    if (error) {
        return makeUnexpected(std::move(*error));
    }
    // Duplicate keys were refused above, so this is the only "data"; migrations change it.
    Value* data = document->find("data");
    if (format == nullptr || version == nullptr || id == nullptr || data == nullptr ||
        format->kind() != Value::Kind::String) {
        return makeUnexpected(headerError("serialization.not_a_document", kNotADocument, source));
    }
    if (format->asString() != kSidecarFormat.name) {
        return makeUnexpected(headerError("serialization.wrong_format",
                                          "the document is not a sidecar ('" + std::string(kSidecarFormat.name) + "')",
                                          source));
    }
    const std::optional<std::uint32_t> number = version->toNumber<std::uint32_t>();
    if (!number) {
        return makeUnexpected(
            headerError("serialization.not_a_document", R"("version" is not a version number)", source));
    }
    if (*number > kSidecarFormat.version) {
        return makeUnexpected(headerError("serialization.newer_version",
                                          "the sidecar has version " + std::to_string(*number) +
                                              ", newer than version " + std::to_string(kSidecarFormat.version) +
                                              " that this engine reads",
                                          source));
    }
    if (*number < kSidecarFormat.minimumVersion) {
        return makeUnexpected(headerError("serialization.version_too_old",
                                          "the sidecar has version " + std::to_string(*number) +
                                              "; the oldest version this engine reads is " +
                                              std::to_string(kSidecarFormat.minimumVersion),
                                          source));
    }
    for (std::uint32_t step = *number; step < kSidecarFormat.version; ++step) {
        Expected<void, serialization::Error> migrated =
            kSidecarFormat.migrations[step - kSidecarFormat.minimumVersion](*data);
        if (!migrated) {
            serialization::Error failure = std::move(migrated).error();
            failure.message = "migration from version " + std::to_string(step) + " failed: " + failure.message;
            failure.code = "serialization.migration_failed";
            failure.source = std::string(source);
            return makeUnexpected(std::move(failure));
        }
    }

    Sidecar sidecar;
    const std::optional<AssetId> assetId = [&]() -> std::optional<AssetId> {
        if (id->kind() != Value::Kind::String) {
            return std::nullopt;
        }
        Expected<AssetId> parsed = AssetId::parse(id->asString());
        return parsed ? std::optional<AssetId>(*parsed) : std::nullopt;
    }();
    if (!assetId) {
        return makeUnexpected(invalid(R"("id" is not a UUID)", source));
    }
    sidecar.id = *assetId;
    if (std::optional<serialization::Error> dataError = readData(*data, source, warnings, sidecar)) {
        return makeUnexpected(std::move(*dataError));
    }
    if (std::optional<serialization::Error> ruleError = checkRules(sidecar, false, source)) {
        return makeUnexpected(std::move(*ruleError));
    }
    std::ranges::sort(sidecar.subAssets, lessByKey);
    return sidecar;
}

std::vector<SubAssetEntry> reconcileSubAssets(std::span<const SubAssetEntry> table, std::span<const std::string> keys) {
    std::unordered_map<std::string_view, const SubAssetEntry*> byKey;
    for (const SubAssetEntry& entry : table) {
        byKey.emplace(entry.key, &entry);
    }
    std::unordered_set<std::string_view> present;
    std::vector<SubAssetEntry> result;
    result.reserve(table.size() + keys.size());
    for (const std::string& key : keys) {
        present.insert(key);
        const auto found = byKey.find(key);
        const SubAssetId id = found != byKey.end() ? found->second->id : SubAssetId::generate();
        result.push_back(SubAssetEntry{.key = key, .id = id, .missing = false});
    }
    for (const SubAssetEntry& entry : table) {
        if (!present.contains(entry.key)) {
            result.push_back(SubAssetEntry{.key = entry.key, .id = entry.id, .missing = true});
        }
    }
    std::ranges::sort(result, lessByKey);
    return result;
}

} // namespace ith
