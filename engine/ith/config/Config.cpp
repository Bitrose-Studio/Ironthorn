#include "ith/config/Config.h"

#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/Assert.h"
#include "ith/reflection/OwnedValue.h"
#include "ith/serialization/Error.h"
#include "ith/serialization/TextArchive.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace ith {
namespace {

using serialization::Value;

Error fromSerialization(const serialization::Error& error) {
    return Error{.code = error.code, .message = error.toString()};
}

// The first key that appears twice in one object of `value`, searching depth first; empty when
// there is none. JSON leaves duplicates undefined, so a file that has one is skipped.
std::string findDuplicateKey(const Value& value) {
    if (value.kind() == Value::Kind::Array) {
        for (const Value& element : value.elements()) {
            if (std::string key = findDuplicateKey(element); !key.empty()) {
                return key;
            }
        }
        return {};
    }
    if (value.kind() != Value::Kind::Object) {
        return {};
    }
    std::set<std::string_view> keys;
    for (const Value::Member& member : value.members()) {
        if (!keys.insert(member.key).second) {
            return member.key;
        }
        if (std::string key = findDuplicateKey(member.value); !key.empty()) {
            return key;
        }
    }
    return {};
}

// The members of `next` that differ from `base` (both written by toValue): a merge patch that
// turns `base` into `next`, recursing into objects. A null in `next` can't be expressed and is
// left out.
Value differenceOf(const Value& base, const Value& next) {
    Value patch = Value::object();
    for (const Value::Member& member : next.members()) {
        const Value* below = base.find(member.key);
        if (below != nullptr && *below == member.value) {
            continue;
        }
        if (member.value.kind() == Value::Kind::Null) {
            continue;
        }
        if (below != nullptr && below->kind() == Value::Kind::Object && member.value.kind() == Value::Kind::Object) {
            Value nested = differenceOf(*below, member.value);
            if (!nested.members().empty()) {
                patch.set(member.key, std::move(nested));
            }
            continue;
        }
        patch.set(member.key, member.value);
    }
    return patch;
}

// True when `relative` names something inside a layer directory.
bool isInsideLayer(std::string_view relative) {
    if (fs::isAbsolutePath(relative)) {
        return false;
    }
    const std::string normalized = fs::normalizePath(relative);
    return normalized != ".." && !normalized.starts_with("../");
}

Unexpected<Error> invalidPath(std::string_view relative) {
    return makeError("config.invalid_path",
                     "'" + std::string(relative) + "' is not a path inside a configuration layer");
}

} // namespace

void applyMergePatch(Value& target, const Value& patch) {
    if (patch.kind() != Value::Kind::Object) {
        target = patch;
        return;
    }
    if (target.kind() != Value::Kind::Object) {
        target = Value::object();
    }
    for (const Value::Member& member : patch.members()) {
        if (member.value.kind() == Value::Kind::Null) {
            target.remove(member.key);
            continue;
        }
        if (Value* existing = target.find(member.key); existing != nullptr) {
            applyMergePatch(*existing, member.value);
        } else {
            Value added;
            applyMergePatch(added, member.value);
            target.set(member.key, std::move(added));
        }
    }
}

Config::Config(std::vector<ConfigLayer> layers) : m_layers(std::move(layers)) {
    ITH_ASSERT_MSG(!m_layers.empty(), "a Config needs at least one layer");
    for (std::size_t index = 0; index < m_layers.size(); ++index) {
        for (std::size_t other = index + 1; other < m_layers.size(); ++other) {
            ITH_ASSERT_MSG(m_layers[index].name != m_layers[other].name, "configuration layer names must be unique");
        }
    }
}

std::optional<std::size_t> Config::findLayer(std::string_view name) const noexcept {
    for (std::size_t index = 0; index < m_layers.size(); ++index) {
        if (m_layers[index].name == name) {
            return index;
        }
    }
    return std::nullopt;
}

const Value& Config::data(std::string_view file) {
    return load(file).merged;
}

Value Config::data(std::string_view file, std::size_t belowLayer) {
    ITH_ASSERT_MSG(belowLayer <= m_layers.size(), "Config::data: no such layer");
    return mergeBelow(load(file), belowLayer < m_layers.size() ? belowLayer : m_layers.size());
}

void Config::warn(std::string code, std::string message) {
    std::string key = code + '\n' + message;
    if (!m_warned.insert(std::move(key)).second) {
        return;
    }
    m_warnings.push_back(Error{.code = std::move(code), .message = std::move(message)});
}

std::vector<Error> Config::takeWarnings() {
    return std::exchange(m_warnings, {});
}

void Config::bump(std::string_view file) {
    ++m_generation;
    if (const auto found = m_fileGenerations.find(file); found != m_fileGenerations.end()) {
        ++found->second;
    } else {
        m_fileGenerations.emplace(std::string(file), 1);
    }
}

std::uint64_t Config::generation(std::string_view file) const {
    const auto found = m_fileGenerations.find(file);
    return m_reloads + (found != m_fileGenerations.end() ? found->second : 0);
}

void Config::readLayers(std::string_view file, LoadedFile& loaded) {
    loaded.layers.assign(m_layers.size(), std::nullopt);
    loaded.merged = Value::object();
    const std::string name = std::string(file) + ".json";
    for (std::size_t index = 0; index < m_layers.size(); ++index) {
        const std::string path = fs::joinPath(m_layers[index].directory, name);
        Expected<std::string> text = fs::readFile(path, kMaxConfigFileSize);
        if (!text) {
            if (text.error().code != "fs.not_found") {
                warn("config.file_skipped", "the configuration file is skipped: " + text.error().message);
            }
            continue;
        }
        std::vector<serialization::Error> warnings;
        Expected<Value, serialization::Error> document =
            serialization::readDocument(*text, kConfigFormat,
                                        {.source = path,
                                         .syntax = serialization::JsonSyntax::Config,
                                         .warnings = &warnings,
                                         .maxSize = kMaxConfigFileSize});
        if (!document) {
            warn("config.file_skipped", "the configuration file is skipped: " + document.error().toString());
            continue;
        }
        if (document->kind() != Value::Kind::Object) {
            warn("config.file_skipped", "the configuration file '" + path + "' is skipped: its data is not an object");
            continue;
        }
        if (const std::string duplicate = findDuplicateKey(*document); !duplicate.empty()) {
            std::string message = "the configuration file '" + path;
            message += "' is skipped: the key '";
            message += duplicate;
            message += "' appears twice";
            warn("config.file_skipped", std::move(message));
            continue;
        }
        for (const serialization::Error& warning : warnings) {
            warn(warning.code, warning.toString());
        }
        applyMergePatch(loaded.merged, *document);
        loaded.layers[index] = std::move(*document);
    }
}

Config::LoadedFile& Config::load(std::string_view file) {
    if (const auto found = m_files.find(file); found != m_files.end()) {
        return found->second;
    }
    LoadedFile& loaded = m_files[std::string(file)];
    readLayers(file, loaded);
    return loaded;
}

Value Config::mergeBelow(const LoadedFile& loaded, std::size_t layer) {
    Value merged = Value::object();
    for (std::size_t index = 0; index < layer; ++index) {
        if (const std::optional<Value>& data = loaded.layers[index]; data.has_value()) {
            applyMergePatch(merged, *data);
        }
    }
    return merged;
}

void Config::readSection(const Value* section, std::string_view file, std::string_view sectionName,
                         const TypeInfo& type, void* target, bool report) {
    ITH_ASSERT_MSG(type.kind == TypeKind::Struct, "a configuration section is a reflected struct");
    if (section == nullptr) {
        return;
    }
    const std::string where = std::string(file) + ".json: " + std::string(sectionName);
    if (section->kind() != Value::Kind::Object) {
        if (report) {
            warn("config.wrong_type", where + ": the section is not an object; the defaults apply");
        }
        return;
    }
    for (const Value::Member& member : section->members()) {
        const FieldInfo* field = type.findField(member.key);
        if (field == nullptr || hasFlags(field->flags, ReflectionFlags::Transient)) {
            if (report) {
                warn("config.unknown_field",
                     where + "." + member.key + ": " + std::string(type.name) + " has no such field; it is ignored");
            }
            continue;
        }
        std::vector<serialization::Error> warnings;
        void* fieldValue = static_cast<std::byte*>(target) + field->offset;
        const Expected<void, serialization::Error> read =
            serialization::fromValue(member.value, field->type(), fieldValue, {.warnings = &warnings});
        if (!report) {
            continue;
        }
        if (!read) {
            warn("config.wrong_type",
                 where + "." + member.key + ": " + read.error().toString() + "; the default is kept");
            continue;
        }
        for (const serialization::Error& warning : warnings) {
            warn(warning.code, where + "." + member.key + ": " + warning.toString());
        }
    }
}

void Config::read(std::string_view file, std::string_view section, const TypeInfo& type, void* target) {
    const LoadedFile& loaded = load(file);
    readSection(loaded.merged.find(section), file, section, type, target, true);
}

std::size_t Config::writableLayer(std::string_view name) const {
    const std::optional<std::size_t> layer =
        name.empty() ? std::optional<std::size_t>(m_layers.size() - 1) : findLayer(name);
    ITH_ASSERT_MSG(layer.has_value(), "no configuration layer has this name");
    ITH_ASSERT_MSG(!layer || m_layers[*layer].writable, "the configuration layer is not writable");
    return layer.value_or(m_layers.size() - 1);
}

Expected<void> Config::write(std::string_view file, std::string_view section, const TypeInfo& type, const void* value,
                             std::string_view layer) {
    const std::size_t target = writableLayer(layer);
    LoadedFile& loaded = load(file);

    // What the layers below give: their merge, read into the struct so the defaults count too.
    const Value below = mergeBelow(loaded, target);
    OwnedValue belowValue(type);
    readSection(below.find(section), file, section, type, belowValue.get(), false);
    Expected<Value, serialization::Error> base = serialization::toValue(type, belowValue.get());
    if (!base) {
        return makeUnexpected(fromSerialization(base.error()));
    }
    Expected<Value, serialization::Error> next = serialization::toValue(type, value);
    if (!next) {
        return makeUnexpected(fromSerialization(next.error()));
    }
    Value patch = differenceOf(*base, *next);

    // The layer's section: the fields of the struct become the difference, other keys stay.
    const Value original = loaded.layers[target].value_or(Value::object());
    Value data = original;
    Value sectionValue = Value::object();
    if (const Value* existing = data.find(section); existing != nullptr && existing->kind() == Value::Kind::Object) {
        sectionValue = *existing;
    }
    for (const FieldInfo& field : type.fields) {
        if (hasFlags(field.flags, ReflectionFlags::Transient)) {
            continue;
        }
        if (!field.attributes.formerName.empty()) {
            sectionValue.remove(field.attributes.formerName);
        }
        if (Value* changed = patch.find(field.name); changed != nullptr) {
            sectionValue.set(std::string(field.name), std::move(*changed));
        } else {
            sectionValue.remove(field.name);
        }
    }
    if (sectionValue.members().empty()) {
        data.remove(section);
    } else {
        data.set(std::string(section), std::move(sectionValue));
    }

    if (!(data == original)) {
        Expected<std::string, serialization::Error> text = serialization::writeDocument(kConfigFormat, data);
        if (!text) {
            return makeUnexpected(fromSerialization(text.error()));
        }
        const std::string path = fs::joinPath(m_layers[target].directory, std::string(file) + ".json");
        if (Expected<void> written = fs::writeFileAtomic(path, *text); !written) {
            return written;
        }
        loaded.layers[target] = std::move(data);
        loaded.merged = mergeBelow(loaded, loaded.layers.size());
    }
    bump(file);
    return {};
}

Expected<std::optional<ConfigFile>> Config::findFile(std::string_view relative) {
    if (!isInsideLayer(relative)) {
        return invalidPath(relative);
    }
    for (std::size_t index = m_layers.size(); index-- > 0;) {
        std::string path = fs::joinPath(m_layers[index].directory, relative);
        const Expected<std::optional<fs::FileInfo>> info = fs::fileInfo(path);
        if (!info) {
            warn("config.file_skipped", "the configuration file is skipped: " + info.error().message);
            continue;
        }
        if (info->has_value() && ((*info)->kind == fs::FileKind::File || (*info)->kind == fs::FileKind::FileLink)) {
            return std::optional<ConfigFile>(
                ConfigFile{.name = fs::normalizePath(relative), .layer = index, .path = std::move(path)});
        }
    }
    return std::optional<ConfigFile>();
}

Expected<std::vector<ConfigFile>> Config::listFiles(std::string_view folder) {
    if (!folder.empty() && !isInsideLayer(folder)) {
        return invalidPath(folder);
    }
    std::map<std::string, ConfigFile, std::less<>> byName;
    for (std::size_t index = 0; index < m_layers.size(); ++index) {
        const std::string directory = fs::joinPath(m_layers[index].directory, folder);
        Expected<std::vector<fs::DirectoryEntry>> entries = fs::listDirectory(directory);
        if (!entries) {
            if (entries.error().code != "fs.not_found") {
                warn("config.file_skipped", "the configuration folder is skipped: " + entries.error().message);
            }
            continue;
        }
        for (fs::DirectoryEntry& entry : *entries) {
            const bool isFile = entry.kind == fs::FileKind::File || entry.kind == fs::FileKind::FileLink;
            if (!isFile || !entry.validName || fs::isTemporaryFileName(entry.name)) {
                continue;
            }
            std::string path = fs::joinPath(directory, entry.name);
            ConfigFile found{.name = entry.name, .layer = index, .path = std::move(path)};
            byName.insert_or_assign(std::move(entry.name), std::move(found));
        }
    }
    std::vector<ConfigFile> files;
    files.reserve(byName.size());
    for (auto& [name, file] : byName) {
        files.push_back(std::move(file));
    }
    return files;
}

Expected<void> Config::writeFile(std::string_view layer, std::string_view relative, std::string_view bytes) {
    const std::size_t target = writableLayer(layer);
    if (!isInsideLayer(relative)) {
        return invalidPath(relative);
    }
    if (Expected<void> written = fs::writeFileAtomic(fs::joinPath(m_layers[target].directory, relative), bytes);
        !written) {
        return written;
    }
    bump(fs::normalizePath(relative));
    return {};
}

Expected<void> Config::removeFile(std::string_view layer, std::string_view relative) {
    const std::size_t target = writableLayer(layer);
    if (!isInsideLayer(relative)) {
        return invalidPath(relative);
    }
    if (Expected<void> removed = fs::remove(fs::joinPath(m_layers[target].directory, relative)); !removed) {
        return removed;
    }
    bump(fs::normalizePath(relative));
    return {};
}

void Config::reload() {
    m_warned.clear();
    for (auto& [file, loaded] : m_files) {
        readLayers(file, loaded);
    }
    ++m_reloads;
    ++m_generation;
}

} // namespace ith
